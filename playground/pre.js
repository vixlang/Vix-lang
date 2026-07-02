var VixcWasm = {
    _wasm_ready: false,
    _pending: [],

    init: function() {
        return new Promise(function(resolve, reject) {
            if (VixcWasm._wasm_ready) { resolve(); return; }
            VixcWasm._pending.push({ resolve: resolve, reject: reject });
        });
    }
};

console.log('[vixc-wasm] pre.js loaded, Module=', typeof Module);

Module.onProgress = function(progress) {
    var bar = document.getElementById('loading-bar');
    if (bar) bar.style.width = (progress * 100) + '%';
};

Module.onRuntimeInitialized = function() {
    if (Module._vixc_ready) return;
    Module._vixc_ready = true;
    console.log('[vixc-wasm] onRuntimeInitialized');
    onVixcWasmReady();
};

function onVixcWasmReady() {
    console.log('[vixc-wasm] onVixcWasmReady called');
    VixcWasm._wasm_ready = true;
    VixcWasm._pending.forEach(function(p) { p.resolve(); });
    VixcWasm._pending = [];
    var cb = window.__vixcWasmReady;
    if (typeof cb === 'function') {
        console.log('[vixc-wasm] calling __vixcWasmReady callback');
        cb();
    } else {
        console.log('[vixc-wasm] __vixcWasmReady NOT set yet');
    }
}
