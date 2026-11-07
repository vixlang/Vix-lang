package main

import (
	"archive/tar"
	"archive/zip"
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

func makeArchive(t *testing.T, zipMode bool) ([]byte, string) {
	var b bytes.Buffer
	if zipMode {
		z := zip.NewWriter(&b)
		w, err := z.Create("bin/vixc")
		if err != nil {
			t.Fatal(err)
		}
		_, _ = w.Write([]byte("ok"))
		if err := z.Close(); err != nil {
			t.Fatal(err)
		}
		return b.Bytes(), "zip"
	}
	g := gzip.NewWriter(&b)
	tw := tar.NewWriter(g)
	if err := tw.WriteHeader(&tar.Header{Name: "bin/vixc", Mode: 0755, Size: 2}); err != nil {
		t.Fatal(err)
	}
	_, _ = tw.Write([]byte("ok"))
	_ = tw.Close()
	_ = g.Close()
	return b.Bytes(), "tar.gz"
}

func TestSafeJoinRejectsTraversal(t *testing.T) {
	if _, err := safeJoin(t.TempDir(), "../../bad"); err == nil {
		t.Fatal("path traversal accepted")
	}
}

func TestInstallAndSwitch(t *testing.T) {
	root := t.TempDir()
	t.Setenv("VIXUP_HOME", root)
	data, format := makeArchive(t, false)
	sum := sha256.Sum256(data)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { _, _ = w.Write(data) }))
	defer server.Close()
	err := install("1.0.0", "stable", Artifact{Target: target(), URL: server.URL, SHA256: hex.EncodeToString(sum[:]), Size: int64(len(data)), Format: format})
	if err != nil {
		t.Fatal(err)
	}
	if err := defaultCmd("1.0.0"); err != nil {
		t.Fatal(err)
	}
	c, err := loadConfig()
	if err != nil || c.DefaultToolchain != "1.0.0" {
		t.Fatalf("config: %#v %v", c, err)
	}
	if _, err := os.Stat(filepath.Join(root, "bin", "vixc")); err != nil {
		t.Fatal(err)
	}
}

func TestChecksumFailureLeavesNoInstall(t *testing.T) {
	root := t.TempDir()
	t.Setenv("VIXUP_HOME", root)
	data, format := makeArchive(t, true)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { _, _ = w.Write(data) }))
	defer server.Close()
	err := install("bad", "stable", Artifact{Target: target(), URL: server.URL, SHA256: "00", Format: format})
	if err == nil {
		t.Fatal("expected checksum error")
	}
	if _, err := os.Stat(toolchainDir("bad")); !os.IsNotExist(err) {
		t.Fatalf("partial install exists: %v", err)
	}
}
