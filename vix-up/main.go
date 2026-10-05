package main

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"time"
)

const version = "0.1.0"

type Config struct {
	DefaultToolchain string `json:"defaultToolchain,omitempty"`
	DistServer       string `json:"distServer,omitempty"`
}
type Manifest struct {
	Schema    int        `json:"schema"`
	Version   string     `json:"version"`
	Channel   string     `json:"channel"`
	Artifacts []Artifact `json:"artifacts"`
}
type Artifact struct {
	Target string `json:"target"`
	URL    string `json:"url"`
	SHA256 string `json:"sha256"`
	Size   int64  `json:"size,omitempty"`
	Format string `json:"format"`
}
type Installed struct {
	Version     string `json:"version"`
	Channel     string `json:"channel,omitempty"`
	Target      string `json:"target"`
	InstalledAt string `json:"installedAt"`
}

func home() string {
	if v := os.Getenv("VIXUP_HOME"); v != "" {
		return v
	}
	h, e := os.UserHomeDir()
	if e != nil {
		return ".vixup"
	}
	if runtime.GOOS == "windows" {
		if v := os.Getenv("LOCALAPPDATA"); v != "" {
			return filepath.Join(v, "Vixup")
		}
	}
	return filepath.Join(h, ".vixup")
}
func dirs() (string, string, string) {
	h := home()
	return filepath.Join(h, "toolchains"), filepath.Join(h, "bin"), filepath.Join(h, "downloads")
}
func configPath() string { return filepath.Join(home(), "config.json") }
func loadConfig() (Config, error) {
	var c Config
	b, e := os.ReadFile(configPath())
	if os.IsNotExist(e) {
		return c, nil
	}
	if e != nil {
		return c, e
	}
	return c, json.Unmarshal(b, &c)
}
func saveConfig(c Config) error {
	if e := os.MkdirAll(home(), 0755); e != nil {
		return e
	}
	b, e := json.MarshalIndent(c, "", "  ")
	if e != nil {
		return e
	}
	return os.WriteFile(configPath(), append(b, '\n'), 0644)
}
func target() string {
	switch {
	case runtime.GOOS == "darwin" && runtime.GOARCH == "arm64":
		return "aarch64-apple-darwin"
	case runtime.GOOS == "darwin" && runtime.GOARCH == "amd64":
		return "x86_64-apple-darwin"
	case runtime.GOOS == "linux" && runtime.GOARCH == "amd64":
		return "x86_64-unknown-linux-gnu"
	case runtime.GOOS == "linux" && runtime.GOARCH == "arm64":
		return "aarch64-unknown-linux-gnu"
	case runtime.GOOS == "windows" && runtime.GOARCH == "amd64":
		return "x86_64-pc-windows-msvc"
	default:
		return runtime.GOARCH + "-" + runtime.GOOS
	}
}
func exe(n string) string {
	if runtime.GOOS == "windows" {
		return n + ".exe"
	}
	return n
}
func toolchainDir(v string) string { t, _, _ := dirs(); return filepath.Join(t, v) }
func resolve(v string, c Config) string {
	if v == "" {
		v = c.DefaultToolchain
	}
	if v == "" {
		v = "stable"
	}
	if _, e := os.Stat(toolchainDir(v)); e == nil {
		return v
	}
	items, _ := installedVersions()
	best := ""
	for _, x := range items {
		if x.Channel == v && x.Version > best {
			best = x.Version
		}
	}
	if best != "" {
		return best
	}
	return v
}
func installedVersions() ([]Installed, error) {
	t, _, _ := dirs()
	es, e := os.ReadDir(t)
	if os.IsNotExist(e) {
		return []Installed{}, nil
	}
	if e != nil {
		return nil, e
	}
	out := []Installed{}
	for _, x := range es {
		if !x.IsDir() {
			continue
		}
		b, e := os.ReadFile(filepath.Join(t, x.Name(), "manifest.json"))
		if e != nil {
			continue
		}
		var i Installed
		if json.Unmarshal(b, &i) == nil {
			out = append(out, i)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Version < out[j].Version })
	return out, nil
}
func fetch(url string) ([]byte, error) {
	r, e := (&http.Client{Timeout: 2 * time.Minute}).Get(url)
	if e != nil {
		return nil, e
	}
	defer r.Body.Close()
	if r.StatusCode != 200 {
		return nil, fmt.Errorf("download %s: %s", url, r.Status)
	}
	return io.ReadAll(io.LimitReader(r.Body, 512<<20))
}
func checksum(p string) (string, int64, error) {
	f, e := os.Open(p)
	if e != nil {
		return "", 0, e
	}
	defer f.Close()
	h := sha256.New()
	n, e := io.Copy(h, f)
	return hex.EncodeToString(h.Sum(nil)), n, e
}
func safeJoin(root, name string) (string, error) {
	name = strings.ReplaceAll(name, "\\", "/")
	name = strings.TrimPrefix(name, "/")
	clean := filepath.Clean(filepath.FromSlash(name))
	if clean == ".." || strings.HasPrefix(clean, ".."+string(os.PathSeparator)) {
		return "", errors.New("archive contains path traversal")
	}
	return filepath.Join(root, clean), nil
}
func extractTarGz(path, dest string) error {
	f, e := os.Open(path)
	if e != nil {
		return e
	}
	defer f.Close()
	g, e := gzip.NewReader(f)
	if e != nil {
		return e
	}
	defer g.Close()
	tr := tar.NewReader(g)
	for {
		h, e := tr.Next()
		if e == io.EOF {
			break
		}
		if e != nil {
			return e
		}
		p, e := safeJoin(dest, h.Name)
		if e != nil {
			return e
		}
		if h.Typeflag == tar.TypeDir {
			if e = os.MkdirAll(p, 0755); e != nil {
				return e
			}
			continue
		}
		if h.Typeflag != tar.TypeReg {
			continue
		}
		if e = os.MkdirAll(filepath.Dir(p), 0755); e != nil {
			return e
		}
		out, e := os.OpenFile(p, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, os.FileMode(h.Mode)&0777)
		if e != nil {
			return e
		}
		_, e = io.Copy(out, io.LimitReader(tr, 512<<20))
		ce := out.Close()
		if e != nil {
			return e
		}
		if ce != nil {
			return ce
		}
	}
	return nil
}
func extractZip(path, dest string) error {
	z, e := zip.OpenReader(path)
	if e != nil {
		return e
	}
	defer z.Close()
	for _, f := range z.File {
		p, e := safeJoin(dest, f.Name)
		if e != nil {
			return e
		}
		if f.FileInfo().IsDir() {
			if e = os.MkdirAll(p, 0755); e != nil {
				return e
			}
			continue
		}
		if e = os.MkdirAll(filepath.Dir(p), 0755); e != nil {
			return e
		}
		in, e := f.Open()
		if e != nil {
			return e
		}
		out, e := os.OpenFile(p, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0644)
		if e != nil {
			in.Close()
			return e
		}
		_, e = io.Copy(out, io.LimitReader(in, 512<<20))
		in.Close()
		ce := out.Close()
		if e != nil {
			return e
		}
		if ce != nil {
			return ce
		}
	}
	return nil
}
func install(v, ch string, a Artifact) error {
	_, _, downloads := dirs()
	if e := os.MkdirAll(downloads, 0755); e != nil {
		return e
	}
	b, e := fetch(a.URL)
	if e != nil {
		return e
	}
	tmp := filepath.Join(downloads, ".download")
	if e = os.WriteFile(tmp, b, 0600); e != nil {
		return e
	}
	defer os.Remove(tmp)
	sum, n, e := checksum(tmp)
	if e != nil {
		return e
	}
	if a.Size > 0 && n != a.Size {
		return fmt.Errorf("size mismatch: got %d, expected %d", n, a.Size)
	}
	if a.SHA256 != "" && strings.ToLower(sum) != strings.ToLower(a.SHA256) {
		return fmt.Errorf("sha256 mismatch: got %s, expected %s", sum, a.SHA256)
	}
	stage := filepath.Join(downloads, ".stage")
	os.RemoveAll(stage)
	if e = os.MkdirAll(stage, 0700); e != nil {
		return e
	}
	defer os.RemoveAll(stage)
	if strings.ToLower(a.Format) == "zip" || strings.HasSuffix(a.URL, ".zip") {
		e = extractZip(tmp, stage)
	} else {
		e = extractTarGz(tmp, stage)
	}
	if e != nil {
		return e
	}
	tc := toolchainDir(v)
	if e = os.MkdirAll(filepath.Dir(tc), 0755); e != nil {
		return e
	}
	old := tc + ".old"
	os.RemoveAll(old)
	if _, e = os.Stat(tc); e == nil {
		if e = os.Rename(tc, old); e != nil {
			return e
		}
	}
	if e = os.Rename(stage, tc); e != nil {
		os.Rename(old, tc)
		return e
	}
	os.RemoveAll(old)
	return writeJSON(filepath.Join(tc, "manifest.json"), Installed{v, ch, target(), time.Now().UTC().Format(time.RFC3339)})
}
func writeJSON(p string, v any) error {
	b, e := json.MarshalIndent(v, "", "  ")
	if e != nil {
		return e
	}
	return os.WriteFile(p, append(b, '\n'), 0644)
}
func manifestURL(server, ch string) string {
	server = strings.TrimRight(server, "/")
	if strings.HasSuffix(server, "manifest.json") {
		return server
	}
	return server + "/" + ch + "/manifest.json"
}
func installCmd(a []string) error {
	if len(a) != 1 {
		return errors.New("usage: vix-up toolchain install <version|channel>")
	}
	c, e := loadConfig()
	if e != nil {
		return e
	}
	server := c.DistServer
	if server == "" {
		server = os.Getenv("VIXUP_DIST_SERVER")
	}
	if server == "" {
		server = "https://github.com/vixlang/Vix-lang/releases/latest/download"
	}
	b, e := fetch(manifestURL(server, a[0]))
	if e != nil {
		return e
	}
	var m Manifest
	if e = json.Unmarshal(b, &m); e != nil {
		return fmt.Errorf("invalid manifest: %w", e)
	}
	if m.Version == "" {
		return errors.New("manifest has no version")
	}
	for _, x := range m.Artifacts {
		if x.Target == target() {
			if _, e = os.Stat(toolchainDir(m.Version)); e == nil {
				fmt.Println("already installed", m.Version)
				return nil
			}
			fmt.Println("installing", m.Version)
			return install(m.Version, m.Channel, x)
		}
	}
	return fmt.Errorf("no artifact for target %s", target())
}
func defaultCmd(v string) error {
	c, e := loadConfig()
	if e != nil {
		return e
	}
	v = resolve(v, c)
	if _, e = os.Stat(toolchainDir(v)); e != nil {
		return fmt.Errorf("toolchain %q is not installed", v)
	}
	c.DefaultToolchain = v
	if e = saveConfig(c); e != nil {
		return e
	}
	return writeShims()
}
func toolPath(root, name string) string {
	if _, err := os.Stat(filepath.Join(root, "bin", exe(name))); err == nil {
		return filepath.Join(root, "bin", exe(name))
	}
	return filepath.Join(root, exe(name))
}
func writeShims() error {
	_, bin, _ := dirs()
	if err := os.MkdirAll(bin, 0755); err != nil {
		return err
	}
	c, err := loadConfig()
	if err != nil {
		return err
	}
	root := toolchainDir(resolve("", c))
	for _, name := range []string{"vix", "vixc", "vix-analyzer"} {
		target := toolPath(root, name)
		p := filepath.Join(bin, name)
		if runtime.GOOS == "windows" {
			p += ".cmd"
			content := "@echo off\r\n\"" + target + "\" %*\r\n"
			if err = os.WriteFile(p, []byte(content), 0644); err != nil {
				return err
			}
		} else {
			escaped := strings.ReplaceAll(target, "'", "'\\''")
			content := "#!/bin/sh\nexec '" + escaped + "' \"$@\"\n"
			if err = os.WriteFile(p, []byte(content), 0755); err != nil {
				return err
			}
		}
	}
	return nil
}
func show() error {
	c, err := loadConfig()
	if err != nil {
		return err
	}
	fmt.Println("home:", home())
	fmt.Println("target:", target())
	fmt.Println("default:", resolve("", c))
	items, err := installedVersions()
	if err != nil {
		return err
	}
	for _, x := range items {
		mark := " "
		if x.Version == resolve("", c) {
			mark = "*"
		}
		fmt.Printf("%s %s (%s)\n", mark, x.Version, x.Target)
	}
	return nil
}
func uninstall(v string) error {
	c, err := loadConfig()
	if err != nil {
		return err
	}
	if v == resolve("", c) {
		return errors.New("refusing to uninstall the active toolchain")
	}
	if err := os.RemoveAll(toolchainDir(v)); err != nil {
		return err
	}
	fmt.Println("uninstalled", v)
	return nil
}
func doctor() error {
	c, err := loadConfig()
	if err != nil {
		return err
	}
	_, bin, _ := dirs()
	fmt.Println("home:", home())
	fmt.Println("target:", target())
	fmt.Println("default:", resolve("", c))
	if _, err := os.Stat(toolchainDir(resolve("", c))); err != nil {
		return fmt.Errorf("default toolchain missing: %w", err)
	}
	fmt.Println("shim directory:", bin)
	fmt.Println("status: ok")
	return nil
}
func usage() {
	fmt.Printf("vix-up %s\n\nUsage:\n  vix-up show\n  vix-up doctor\n  vix-up update [channel]\n  vix-up default <version|channel>\n  vix-up toolchain list\n  vix-up toolchain install <version|channel>\n  vix-up toolchain uninstall <version>\n\nEnvironment: VIXUP_HOME, VIXUP_DIST_SERVER\n", version)
}
func main() {
	flag.Usage = usage
	flag.Parse()
	args := flag.Args()
	if len(args) == 0 {
		usage()
		return
	}
	var err error
	switch args[0] {
	case "--version", "-V":
		fmt.Println("vix-up", version)
		return
	case "show":
		err = show()
	case "doctor":
		err = doctor()
	case "update":
		channel := "stable"
		if len(args) > 2 {
			err = errors.New("usage: vix-up update [channel]")
		} else {
			if len(args) == 2 {
				channel = args[1]
			}
			err = installCmd([]string{channel})
		}
	case "default":
		if len(args) != 2 {
			err = errors.New("usage: vix-up default <version|channel>")
		} else {
			err = defaultCmd(args[1])
		}
	case "toolchain":
		if len(args) < 2 {
			err = errors.New("missing toolchain command")
		} else {
			switch args[1] {
			case "list":
				var items []Installed
				items, err = installedVersions()
				for _, x := range items {
					fmt.Println(x.Version)
				}
			case "install":
				err = installCmd(args[2:])
			case "uninstall":
				if len(args) != 3 {
					err = errors.New("usage: vix-up toolchain uninstall <version>")
				} else {
					err = uninstall(args[2])
				}
			default:
				err = fmt.Errorf("unknown toolchain command %q", args[1])
			}
		}
	default:
		err = fmt.Errorf("unknown command %q", args[0])
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "vix-up:", err)
		os.Exit(1)
	}
}
