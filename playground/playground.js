(function() {
    var EXAMPLES = {
        hello: 'fn main(): i32 {\n'
            + '    print("Hello, Vix Playground!")\n'
            + '    return 0\n'
            + '}',
        bubble: 'fn sort(nums: [i32], size: i32): i32 {\n'
            + '    for (i in 0 .. size - 1) {\n'
            + '        for (j in 0 .. size - i - 1) {\n'
            + '            if (nums[j] > nums[j + 1]) {\n'
            + '                let temp = nums[j]\n'
            + '                nums[j] = nums[j + 1]\n'
            + '                nums[j + 1] = temp\n'
            + '            }\n'
            + '        }\n'
            + '    }\n'
            + '    return 0\n'
            + '}\n'
            + 'fn main(): i32 {\n'
            + '    let arr = [5, 2, 8, 1, 9, 10]\n'
            + '    sort(arr, arr.length)\n'
            + '    for (i in 0 .. arr.length) {\n'
            + '        print(arr[i])\n'
            + '    }\n'
            + '    return 0\n'
            + '}',
        struct: 'struct Point {\n'
            + '    x: i32,\n'
            + '    y: i32,\n'
            + '}\n'
            + 'fn main(): i32 {\n'
            + '    let p = Point { x: 3, y: 4 }\n'
            + '    print(p.x)\n'
            + '    print(p.y)\n'
            + '    return 0\n'
            + '}',
        'binary-search': 'fn binary_search(arr: [i32], target: i32): i32 {\n'
            + '    let mut lo = 0\n'
            + '    let mut hi = arr.length - 1\n'
            + '    while (lo <= hi) {\n'
            + '        let mid = lo + (hi - lo) / 2\n'
            + '        if (arr[mid] == target) { return mid }\n'
            + '        elif (arr[mid] < target) { lo = mid + 1 }\n'
            + '        else { hi = mid - 1 }\n'
            + '    }\n'
            + '    return -1\n'
            + '}\n'
            + 'fn main(): i32 {\n'
            + '    let arr = [2, 5, 8, 12, 16, 23, 38, 56, 72, 91]\n'
            + '    print(binary_search(arr, 23))\n'
            + '    print(binary_search(arr, 56))\n'
            + '    print(binary_search(arr, 100))\n'
            + '    return 0\n'
            + '}',
    };

    var saved = localStorage.getItem('vix-playground-code');
    var initialCode = saved && EXAMPLES[saved] ? EXAMPLES[saved] : (saved || EXAMPLES.hello);

    var editor = CodeMirror(document.getElementById('editor-container'), {
        value: initialCode,
        mode: 'text/x-vix',
        theme: 'default',
        lineNumbers: true,
        indentUnit: 4,
        tabSize: 4,
        autofocus: true,
        extraKeys: {
            'Ctrl-Enter': runCode,
            'Cmd-Enter': runCode
        }
    });

    editor.on('change', function() {
        localStorage.setItem('vix-playground-code', editor.getValue());
    });

    var statusText = document.getElementById('status-text');
    var runBtn = document.getElementById('btn-run');
    var exampleSel = document.getElementById('example-selector');
    var loadingOverlay = document.getElementById('loading-overlay');

    exampleSel.addEventListener('change', function() {
        var code = EXAMPLES[this.value];
        if (code) {
            editor.setValue(code);
            localStorage.setItem('vix-playground-code', code);
        }
    });

    var wasmModule = null;
    var outputText = '';

    function flushOutput() {
        var el = document.getElementById('output-content');
        el.textContent = outputText;
        el.scrollTop = el.scrollHeight;
    }

    function formatCompileError(raw) {
        var lines = raw.split('\n');
        for (var i = 0; i < lines.length; i++) {
            if (lines[i].includes('SyntaxError') || lines[i].includes('TypeError')) {
                var m = lines[i].match(/:(\d+):(\d+)/);
                if (m) {
                    var line = parseInt(m[1]);
                    var col = parseInt(m[2]);
                    return '行 ' + line + ', 列 ' + col + ': ' + lines[i];
                }
                return lines[i];
            }
        }
        return raw || '未知错误';
    }

    function createWasmImports() {
        var env = {
            vix_putchar: function(c) {
                outputText += String.fromCharCode(c);
                if (c === 10) flushOutput();
            },
            vix_puts: function(ptr) {
                var mem = new Uint8Array(wasmModule.instance.exports.memory.buffer);
                var s = '';
                while (mem[ptr] !== 0) {
                    s += String.fromCharCode(mem[ptr]);
                    ptr++;
                }
                outputText += s + '\n';
                flushOutput();
            },
            vix_print_i32: function(value) {
                outputText += String(value) + '\n';
                flushOutput();
            },
            vix_exit: function(code) {
                console.log('Program exited with code', code);
            }
        };
        return { env: env };
    }

    async function runCode() {
        var source = editor.getValue();
        var outputEl = document.getElementById('output-content');
        outputText = '';
        outputEl.textContent = '编译中...\n';
        console.log('[playground] runCode starting, source len:', source.length);

        try {
            // 分配输出参数指针的内存 (32-bit wasm: 每个指针 4 字节)
            var ptrOut = Module._malloc(4);
            var lenOut = Module._malloc(4);
            var errOut = Module._malloc(4);
            console.log('[playground] allocated ptrOut lenOut errOut');

            // 初始化为 0
            Module.setValue(ptrOut, 0, 'i32');
            Module.setValue(lenOut, 0, 'i32');
            Module.setValue(errOut, 0, 'i32');

            console.log('[playground] calling compile_vix...');
            var success = Module.ccall('compile_vix',
                'number', ['string', 'number', 'number', 'number'],
                [source, ptrOut, lenOut, errOut]);
            console.log('[playground] compile_vix returned:', success);

            if (!success) {
                var errPtr = Module.getValue(errOut, 'i32');
                var errorMsg = errPtr ? Module.UTF8ToString(errPtr) : '编译失败';
                console.log('[playground] compile failed:', errorMsg);
                outputEl.textContent = formatCompileError(errorMsg);
                Module.ccall('free_wasm_result', null, ['number', 'number'], [0, errPtr]);
                Module._free(ptrOut);
                Module._free(lenOut);
                Module._free(errOut);
                return;
            }

            console.log('[playground] reading ptr/len from outputs...');
            var ptr = Module.getValue(ptrOut, 'i32');
            var len = Module.getValue(lenOut, 'i32');
            console.log('[playground] ptr:', ptr, 'len:', len, 'HEAPU8 type:', typeof HEAPU8);

            console.log('[playground] slicing wasm bytes...');
            var wasmBytes = HEAPU8.slice(ptr, ptr + len);
            console.log('[playground] wasmBytes length:', wasmBytes.length);

            console.log('[playground] freeing compiler result...');
            Module.ccall('free_wasm_result', null, ['number', 'number'], [ptr, 0]);

            Module._free(ptrOut);
            Module._free(lenOut);
            Module._free(errOut);

            console.log('[playground] creating import object...');
            var importObj = createWasmImports();

            console.log('[playground] instantiating wasm module...');
            wasmModule = await WebAssembly.instantiate(wasmBytes, importObj);
            console.log('[playground] instantiation done, exports:', Object.keys(wasmModule.instance.exports));

            outputText = '';
            console.log('[playground] calling main...');
            wasmModule.instance.exports.main();
            console.log('[playground] main returned, outputText:', outputText);
            outputEl.textContent = outputText || '(无输出)';
        } catch (err) {
            console.log('[playground] ERROR:', err.message, err.stack);
            outputEl.textContent += '运行时错误: ' + err.message + '\n';
        }
    }

    runBtn.disabled = true;
    statusText.textContent = '正在加载编译器 (5-10 MB)...';

    window.onVixcWasmReady = function() {
        console.log('[playground] onVixcWasmReady fired');
        statusText.textContent = '编译器就绪';
        runBtn.disabled = false;
        if (loadingOverlay) loadingOverlay.style.display = 'none';
    };

    // 安全 fallback: 10 秒后如果还没就绪，强制隐藏加载覆盖层
    setTimeout(function() {
        if (loadingOverlay && loadingOverlay.style.display !== 'none') {
            console.log('[playground] fallback: forcing loading overlay hidden');
            loadingOverlay.style.display = 'none';
            statusText.textContent = '编译器加载超时, 可能不可用';
        }
    }, 15000);

    document.querySelectorAll('[data-tab]').forEach(function(btn) {
        btn.addEventListener('click', function() {
            document.querySelectorAll('[data-tab]').forEach(function(b) { b.classList.remove('active'); });
            this.classList.add('active');
            var tab = this.dataset.tab;
            document.getElementById('output-content').style.display = tab === 'output' ? 'block' : 'none';
            document.getElementById('wasm-content').style.display = tab === 'wasm' ? 'block' : 'none';
        });
    });

    runBtn.addEventListener('click', runCode);
})();
