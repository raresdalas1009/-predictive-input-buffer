let wasm;

const heap = new Array(128).fill(undefined);

heap.push(undefined, null, true, false);

function getObject(idx) { return heap[idx]; }

let heap_next = heap.length;

function dropObject(idx) {
    if (idx < 132) return;
    heap[idx] = heap_next;
    heap_next = idx;
}

function takeObject(idx) {
    const ret = getObject(idx);
    dropObject(idx);
    return ret;
}

const cachedTextDecoder = (typeof TextDecoder !== 'undefined' ? new TextDecoder('utf-8', { ignoreBOM: true, fatal: true }) : { decode: () => { throw Error('TextDecoder not available') } } );

if (typeof TextDecoder !== 'undefined') { cachedTextDecoder.decode(); };

let cachedUint8Memory0 = null;

function getUint8Memory0() {
    if (cachedUint8Memory0 === null || cachedUint8Memory0.byteLength === 0) {
        cachedUint8Memory0 = new Uint8Array(wasm.memory.buffer);
    }
    return cachedUint8Memory0;
}

function getStringFromWasm0(ptr, len) {
    ptr = ptr >>> 0;
    return cachedTextDecoder.decode(getUint8Memory0().subarray(ptr, ptr + len));
}

function addHeapObject(obj) {
    if (heap_next === heap.length) heap.push(heap.length + 1);
    const idx = heap_next;
    heap_next = heap[idx];

    heap[idx] = obj;
    return idx;
}
/**
*/
export function run() {
    wasm.run();
}

function _assertClass(instance, klass) {
    if (!(instance instanceof klass)) {
        throw new Error(`expected instance of ${klass.name}`);
    }
    return instance.ptr;
}

let cachedInt32Memory0 = null;

function getInt32Memory0() {
    if (cachedInt32Memory0 === null || cachedInt32Memory0.byteLength === 0) {
        cachedInt32Memory0 = new Int32Array(wasm.memory.buffer);
    }
    return cachedInt32Memory0;
}

let cachedUint32Memory0 = null;

function getUint32Memory0() {
    if (cachedUint32Memory0 === null || cachedUint32Memory0.byteLength === 0) {
        cachedUint32Memory0 = new Uint32Array(wasm.memory.buffer);
    }
    return cachedUint32Memory0;
}

function getArrayJsValueFromWasm0(ptr, len) {
    ptr = ptr >>> 0;
    const mem = getUint32Memory0();
    const slice = mem.subarray(ptr / 4, ptr / 4 + len);
    const result = [];
    for (let i = 0; i < slice.length; i++) {
        result.push(takeObject(slice[i]));
    }
    return result;
}

let WASM_VECTOR_LEN = 0;

const cachedTextEncoder = (typeof TextEncoder !== 'undefined' ? new TextEncoder('utf-8') : { encode: () => { throw Error('TextEncoder not available') } } );

const encodeString = (typeof cachedTextEncoder.encodeInto === 'function'
    ? function (arg, view) {
    return cachedTextEncoder.encodeInto(arg, view);
}
    : function (arg, view) {
    const buf = cachedTextEncoder.encode(arg);
    view.set(buf);
    return {
        read: arg.length,
        written: buf.length
    };
});

function passStringToWasm0(arg, malloc, realloc) {

    if (realloc === undefined) {
        const buf = cachedTextEncoder.encode(arg);
        const ptr = malloc(buf.length, 1) >>> 0;
        getUint8Memory0().subarray(ptr, ptr + buf.length).set(buf);
        WASM_VECTOR_LEN = buf.length;
        return ptr;
    }

    let len = arg.length;
    let ptr = malloc(len, 1) >>> 0;

    const mem = getUint8Memory0();

    let offset = 0;

    for (; offset < len; offset++) {
        const code = arg.charCodeAt(offset);
        if (code > 0x7F) break;
        mem[ptr + offset] = code;
    }

    if (offset !== len) {
        if (offset !== 0) {
            arg = arg.slice(offset);
        }
        ptr = realloc(ptr, len, len = offset + arg.length * 3, 1) >>> 0;
        const view = getUint8Memory0().subarray(ptr + offset, ptr + len);
        const ret = encodeString(arg, view);

        offset += ret.written;
        ptr = realloc(ptr, len, offset, 1) >>> 0;
    }

    WASM_VECTOR_LEN = offset;
    return ptr;
}

function passArray8ToWasm0(arg, malloc) {
    const ptr = malloc(arg.length * 1, 1) >>> 0;
    getUint8Memory0().set(arg, ptr / 1);
    WASM_VECTOR_LEN = arg.length;
    return ptr;
}

function getArrayU8FromWasm0(ptr, len) {
    ptr = ptr >>> 0;
    return getUint8Memory0().subarray(ptr / 1, ptr / 1 + len);
}

function isLikeNone(x) {
    return x === undefined || x === null;
}

function handleError(f, args) {
    try {
        return f.apply(this, args);
    } catch (e) {
        wasm.__wbindgen_exn_store(addHeapObject(e));
    }
}
/**
*/
export const ClickType = Object.freeze({ Click:0,"0":"Click",Release:1,"1":"Release",Skip:2,"2":"Skip", });
/**
*/
export const GameVersion = Object.freeze({ Any:0,"0":"Any",Version2113:1,"1":"Version2113",Version2206:2,"2":"Version2206", });
/**
*/
export const Format = Object.freeze({ OmegaBot:0,"0":"OmegaBot",OmegaBot2:1,"1":"OmegaBot2",URL:2,"2":"URL",MHRJson:3,"3":"MHRJson",MHRBinary:4,"4":"MHRBinary",Tasbot:5,"5":"Tasbot",ZBot:6,"6":"ZBot",ReplayBot:7,"7":"ReplayBot",Fembot:8,"8":"Fembot",EchoOld:9,"9":"EchoOld",EchoNewJson:10,"10":"EchoNewJson",EchoNewBinary:11,"11":"EchoNewBinary",YBot1:12,"12":"YBot1",XBot:13,"13":"XBot",Rush:14,"14":"Rush",KDBot:15,"15":"KDBot",YBot:16,"16":"YBot",GDR:17,"17":"GDR",GDRJson:18,"18":"GDRJson",XDBot:19,"19":"XDBot",Silicate:20,"20":"Silicate",Slc2:21,"21":"Slc2",Slc3:22,"22":"Slc3",Tcm:23,"23":"Tcm",PlainText:24,"24":"PlainText",GDR2:25,"25":"GDR2",AstV9:26,"26":"AstV9",Amethyst:27,"27":"Amethyst",OsuReplay:28,"28":"OsuReplay",Gdmo:29,"29":"Gdmo",Ddhor:30,"30":"Ddhor",ReplayEngine:31,"31":"ReplayEngine",ReplayEngine2:32,"32":"ReplayEngine2",ReplayEngine3:33,"33":"ReplayEngine3",ReplayEngine4:34,"34":"ReplayEngine4",QBot:35,"35":"QBot",RBot:36,"36":"RBot",Zephyrus:37,"37":"Zephyrus",UvBot:38,"38":"UvBot",Ttr:39,"39":"Ttr",Ttr2:40,"40":"Ttr2",Ttr3:41,"41":"Ttr3",Ttrl:42,"42":"Ttrl",Cml:43,"43":"Cml",CmlV5:44,"44":"CmlV5",CmlV6:45,"45":"CmlV6",CmlV7:46,"46":"CmlV7",Grape:47,"47":"Grape",
/**
* Vine v3. Parsing only -- `save` will fail.
*/
Vine:48,"48":"Vine", });
/**
*/
export const ConverterError = Object.freeze({ InvalidData:0,"0":"InvalidData", });
/**
*/
export const CPSRule = Object.freeze({ Rule16CPS:0,"0":"Rule16CPS",Rule3CPF:1,"1":"Rule3CPF",Rule45CP5C:2,"2":"Rule45CP5C", });

const CPSViolationFinalization = (typeof FinalizationRegistry === 'undefined')
    ? { register: () => {}, unregister: () => {} }
    : new FinalizationRegistry(ptr => wasm.__wbg_cpsviolation_free(ptr >>> 0));
/**
*/
export class CPSViolation {

    __destroy_into_raw() {
        const ptr = this.__wbg_ptr;
        this.__wbg_ptr = 0;
        CPSViolationFinalization.unregister(this);
        return ptr;
    }

    free() {
        const ptr = this.__destroy_into_raw();
        wasm.__wbg_cpsviolation_free(ptr);
    }
    /**
    * @returns {CPSRule}
    */
    get rule() {
        const ret = wasm.__wbg_get_cpsviolation_rule(this.__wbg_ptr);
        return ret;
    }
    /**
    * @param {CPSRule} arg0
    */
    set rule(arg0) {
        wasm.__wbg_set_cpsviolation_rule(this.__wbg_ptr, arg0);
    }
    /**
    * @returns {number}
    */
    get frame() {
        const ret = wasm.__wbg_get_cpsviolation_frame(this.__wbg_ptr);
        return ret >>> 0;
    }
    /**
    * @param {number} arg0
    */
    set frame(arg0) {
        wasm.__wbg_set_cpsviolation_frame(this.__wbg_ptr, arg0);
    }
    /**
    * @returns {number}
    */
    get cps() {
        const ret = wasm.__wbg_get_cpsviolation_cps(this.__wbg_ptr);
        return ret;
    }
    /**
    * @param {number} arg0
    */
    set cps(arg0) {
        wasm.__wbg_set_cpsviolation_cps(this.__wbg_ptr, arg0);
    }
}

const ClickFinalization = (typeof FinalizationRegistry === 'undefined')
    ? { register: () => {}, unregister: () => {} }
    : new FinalizationRegistry(ptr => wasm.__wbg_click_free(ptr >>> 0));
/**
*/
export class Click {

    static __wrap(ptr) {
        ptr = ptr >>> 0;
        const obj = Object.create(Click.prototype);
        obj.__wbg_ptr = ptr;
        ClickFinalization.register(obj, obj.__wbg_ptr, obj);
        return obj;
    }

    __destroy_into_raw() {
        const ptr = this.__wbg_ptr;
        this.__wbg_ptr = 0;
        ClickFinalization.unregister(this);
        return ptr;
    }

    free() {
        const ptr = this.__destroy_into_raw();
        wasm.__wbg_click_free(ptr);
    }
    /**
    * @returns {number}
    */
    get frame() {
        const ret = wasm.__wbg_get_click_frame(this.__wbg_ptr);
        return ret >>> 0;
    }
    /**
    * @param {number} arg0
    */
    set frame(arg0) {
        wasm.__wbg_set_click_frame(this.__wbg_ptr, arg0);
    }
    /**
    * @returns {ClickType}
    */
    get p1() {
        const ret = wasm.__wbg_get_click_p1(this.__wbg_ptr);
        return ret;
    }
    /**
    * @param {ClickType} arg0
    */
    set p1(arg0) {
        wasm.__wbg_set_click_p1(this.__wbg_ptr, arg0);
    }
    /**
    * @returns {ClickType}
    */
    get p2() {
        const ret = wasm.__wbg_get_click_p2(this.__wbg_ptr);
        return ret;
    }
    /**
    * @param {ClickType} arg0
    */
    set p2(arg0) {
        wasm.__wbg_set_click_p2(this.__wbg_ptr, arg0);
    }
}

const ConverterFinalization = (typeof FinalizationRegistry === 'undefined')
    ? { register: () => {}, unregister: () => {} }
    : new FinalizationRegistry(ptr => wasm.__wbg_converter_free(ptr >>> 0));
/**
*/
export class Converter {

    __destroy_into_raw() {
        const ptr = this.__wbg_ptr;
        this.__wbg_ptr = 0;
        ConverterFinalization.unregister(this);
        return ptr;
    }

    free() {
        const ptr = this.__destroy_into_raw();
        wasm.__wbg_converter_free(ptr);
    }
    /**
    * @returns {Settings}
    */
    get settings() {
        const ret = wasm.__wbg_get_converter_settings(this.__wbg_ptr);
        return Settings.__wrap(ret);
    }
    /**
    * @param {Settings} arg0
    */
    set settings(arg0) {
        _assertClass(arg0, Settings);
        var ptr0 = arg0.__destroy_into_raw();
        wasm.__wbg_set_converter_settings(this.__wbg_ptr, ptr0);
    }
    /**
    */
    flip_p1_p2() {
        wasm.converter_flip_p1_p2(this.__wbg_ptr);
    }
    /**
    */
    flip_up_down() {
        wasm.converter_flip_up_down(this.__wbg_ptr);
    }
    /**
    * @returns {GameVersion}
    */
    game_version() {
        const ret = wasm.converter_game_version(this.__wbg_ptr);
        return ret;
    }
    /**
    * @param {bigint} offset
    */
    offset_all_by(offset) {
        wasm.converter_offset_all_by(this.__wbg_ptr, offset);
    }
    /**
    * @param {number} idx
    * @param {number} page
    * @returns {(Click)[]}
    */
    clicks_at_batch(idx, page) {
        try {
            const retptr = wasm.__wbindgen_add_to_stack_pointer(-16);
            wasm.converter_clicks_at_batch(retptr, this.__wbg_ptr, idx, page);
            var r0 = getInt32Memory0()[retptr / 4 + 0];
            var r1 = getInt32Memory0()[retptr / 4 + 1];
            var v1 = getArrayJsValueFromWasm0(r0, r1).slice();
            wasm.__wbindgen_free(r0, r1 * 4, 4);
            return v1;
        } finally {
            wasm.__wbindgen_add_to_stack_pointer(16);
        }
    }
    /**
    * @param {number} idx
    * @param {number} frame
    */
    insert_empty_at(idx, frame) {
        wasm.converter_insert_empty_at(this.__wbg_ptr, idx, frame);
    }
    /**
    * @param {number} idx
    * @param {boolean} player_2
    */
    toggle_click_at(idx, player_2) {
        wasm.converter_toggle_click_at(this.__wbg_ptr, idx, player_2);
    }
    /**
    * @param {number} idx
    * @param {number} frame
    */
    replace_frame_at(idx, frame) {
        wasm.converter_replace_frame_at(this.__wbg_ptr, idx, frame);
    }
    /**
    * @param {boolean} value
    */
    set_setting_auto_offset(value) {
        wasm.converter_set_setting_auto_offset(this.__wbg_ptr, value);
    }
    /**
    * @param {boolean} player_2
    */
    remove_all_player_inputs(player_2) {
        wasm.converter_remove_all_player_inputs(this.__wbg_ptr, player_2);
    }
    /**
    * @param {boolean} value
    */
    set_setting_beautify_json(value) {
        wasm.converter_set_setting_beautify_json(this.__wbg_ptr, value);
    }
    /**
    * Sets the game version written into Astral V9 (.ast) saves, e.g.
    * "2.2081" or "2.113". Empty leaves the field at the format default.
    * @param {string} value
    */
    set_setting_ast_game_version(value) {
        const ptr0 = passStringToWasm0(value, wasm.__wbindgen_malloc, wasm.__wbindgen_realloc);
        const len0 = WASM_VECTOR_LEN;
        wasm.converter_set_setting_ast_game_version(this.__wbg_ptr, ptr0, len0);
    }
    /**
    */
    constructor() {
        const ret = wasm.converter_new();
        this.__wbg_ptr = ret >>> 0;
        return this;
    }
    /**
    * @param {Uint8Array} data
    * @param {Format} fmt
    */
    load(data, fmt) {
        try {
            const retptr = wasm.__wbindgen_add_to_stack_pointer(-16);
            const ptr0 = passArray8ToWasm0(data, wasm.__wbindgen_malloc);
            const len0 = WASM_VECTOR_LEN;
            wasm.converter_load(retptr, this.__wbg_ptr, ptr0, len0, fmt);
            var r0 = getInt32Memory0()[retptr / 4 + 0];
            var r1 = getInt32Memory0()[retptr / 4 + 1];
            if (r1) {
                throw takeObject(r0);
            }
        } finally {
            wasm.__wbindgen_add_to_stack_pointer(16);
        }
    }
    /**
    * Returns an error rather than a partial buffer when a writer fails --
    * otherwise a failed conversion silently downloads a 0-byte file.
    * @param {Format} fmt
    * @returns {Uint8Array}
    */
    save(fmt) {
        try {
            const retptr = wasm.__wbindgen_add_to_stack_pointer(-16);
            wasm.converter_save(retptr, this.__wbg_ptr, fmt);
            var r0 = getInt32Memory0()[retptr / 4 + 0];
            var r1 = getInt32Memory0()[retptr / 4 + 1];
            var r2 = getInt32Memory0()[retptr / 4 + 2];
            var r3 = getInt32Memory0()[retptr / 4 + 3];
            if (r3) {
                throw takeObject(r2);
            }
            var v1 = getArrayU8FromWasm0(r0, r1).slice();
            wasm.__wbindgen_free(r0, r1 * 1, 1);
            return v1;
        } finally {
            wasm.__wbindgen_add_to_stack_pointer(16);
        }
    }
    /**
    */
    sort() {
        wasm.converter_sort(this.__wbg_ptr);
    }
    /**
    */
    clean() {
        wasm.converter_clean(this.__wbg_ptr);
    }
    /**
    * @returns {(Click)[]}
    */
    clicks() {
        try {
            const retptr = wasm.__wbindgen_add_to_stack_pointer(-16);
            wasm.converter_clicks(retptr, this.__wbg_ptr);
            var r0 = getInt32Memory0()[retptr / 4 + 0];
            var r1 = getInt32Memory0()[retptr / 4 + 1];
            var v1 = getArrayJsValueFromWasm0(r0, r1).slice();
            wasm.__wbindgen_free(r0, r1 * 4, 4);
            return v1;
        } finally {
            wasm.__wbindgen_add_to_stack_pointer(16);
        }
    }
    /**
    * @returns {number}
    */
    length() {
        const ret = wasm.converter_length(this.__wbg_ptr);
        return ret >>> 0;
    }
    /**
    * @returns {number}
    */
    get_fps() {
        const ret = wasm.converter_get_fps(this.__wbg_ptr);
        return ret;
    }
    /**
    * @param {number} fps
    */
    set_fps(fps) {
        wasm.converter_set_fps(this.__wbg_ptr, fps);
    }
    /**
    * @param {number} idx
    * @returns {Click}
    */
    click_at(idx) {
        const ret = wasm.converter_click_at(this.__wbg_ptr, idx);
        return Click.__wrap(ret);
    }
    /**
    */
    check_cps() {
        wasm.converter_check_cps(this.__wbg_ptr);
    }
    /**
    * @param {number} idx
    */
    remove_at(idx) {
        wasm.converter_remove_at(this.__wbg_ptr, idx);
    }
}

const SettingsFinalization = (typeof FinalizationRegistry === 'undefined')
    ? { register: () => {}, unregister: () => {} }
    : new FinalizationRegistry(ptr => wasm.__wbg_settings_free(ptr >>> 0));
/**
*/
export class Settings {

    static __wrap(ptr) {
        ptr = ptr >>> 0;
        const obj = Object.create(Settings.prototype);
        obj.__wbg_ptr = ptr;
        SettingsFinalization.register(obj, obj.__wbg_ptr, obj);
        return obj;
    }

    __destroy_into_raw() {
        const ptr = this.__wbg_ptr;
        this.__wbg_ptr = 0;
        SettingsFinalization.unregister(this);
        return ptr;
    }

    free() {
        const ptr = this.__destroy_into_raw();
        wasm.__wbg_settings_free(ptr);
    }
    /**
    * @returns {boolean}
    */
    get auto_offset() {
        const ret = wasm.__wbg_get_settings_auto_offset(this.__wbg_ptr);
        return ret !== 0;
    }
    /**
    * @param {boolean} arg0
    */
    set auto_offset(arg0) {
        wasm.__wbg_set_settings_auto_offset(this.__wbg_ptr, arg0);
    }
    /**
    * @returns {boolean}
    */
    get beautified_json() {
        const ret = wasm.__wbg_get_settings_beautified_json(this.__wbg_ptr);
        return ret !== 0;
    }
    /**
    * @param {boolean} arg0
    */
    set beautified_json(arg0) {
        wasm.__wbg_set_settings_beautified_json(this.__wbg_ptr, arg0);
    }
}

async function __wbg_load(module, imports) {
    if (typeof Response === 'function' && module instanceof Response) {
        if (typeof WebAssembly.instantiateStreaming === 'function') {
            try {
                return await WebAssembly.instantiateStreaming(module, imports);

            } catch (e) {
                if (module.headers.get('Content-Type') != 'application/wasm') {
                    console.warn("`WebAssembly.instantiateStreaming` failed because your server does not serve wasm with `application/wasm` MIME type. Falling back to `WebAssembly.instantiate` which is slower. Original error:\n", e);

                } else {
                    throw e;
                }
            }
        }

        const bytes = await module.arrayBuffer();
        return await WebAssembly.instantiate(bytes, imports);

    } else {
        const instance = await WebAssembly.instantiate(module, imports);

        if (instance instanceof WebAssembly.Instance) {
            return { instance, module };

        } else {
            return instance;
        }
    }
}

function __wbg_get_imports() {
    const imports = {};
    imports.wbg = {};
    imports.wbg.__wbindgen_object_drop_ref = function(arg0) {
        takeObject(arg0);
    };
    imports.wbg.__wbg_click_new = function(arg0) {
        const ret = Click.__wrap(arg0);
        return addHeapObject(ret);
    };
    imports.wbg.__wbg_log_5d2f2656ef4f052f = function(arg0, arg1) {
        console.log(getStringFromWasm0(arg0, arg1));
    };
    imports.wbg.__wbg_error_2704ae79e4c75ea3 = function(arg0, arg1) {
        console.error(getStringFromWasm0(arg0, arg1));
    };
    imports.wbg.__wbindgen_error_new = function(arg0, arg1) {
        const ret = new Error(getStringFromWasm0(arg0, arg1));
        return addHeapObject(ret);
    };
    imports.wbg.__wbg_instanceof_Window_f401953a2cf86220 = function(arg0) {
        let result;
        try {
            result = getObject(arg0) instanceof Window;
        } catch (_) {
            result = false;
        }
        const ret = result;
        return ret;
    };
    imports.wbg.__wbg_document_5100775d18896c16 = function(arg0) {
        const ret = getObject(arg0).document;
        return isLikeNone(ret) ? 0 : addHeapObject(ret);
    };
    imports.wbg.__wbg_body_edb1908d3ceff3a1 = function(arg0) {
        const ret = getObject(arg0).body;
        return isLikeNone(ret) ? 0 : addHeapObject(ret);
    };
    imports.wbg.__wbg_new_abda76e883ba8a5f = function() {
        const ret = new Error();
        return addHeapObject(ret);
    };
    imports.wbg.__wbg_stack_658279fe44541cf6 = function(arg0, arg1) {
        const ret = getObject(arg1).stack;
        const ptr1 = passStringToWasm0(ret, wasm.__wbindgen_malloc, wasm.__wbindgen_realloc);
        const len1 = WASM_VECTOR_LEN;
        getInt32Memory0()[arg0 / 4 + 1] = len1;
        getInt32Memory0()[arg0 / 4 + 0] = ptr1;
    };
    imports.wbg.__wbg_error_f851667af71bcfc6 = function(arg0, arg1) {
        let deferred0_0;
        let deferred0_1;
        try {
            deferred0_0 = arg0;
            deferred0_1 = arg1;
            console.error(getStringFromWasm0(arg0, arg1));
        } finally {
            wasm.__wbindgen_free(deferred0_0, deferred0_1, 1);
        }
    };
    imports.wbg.__wbg_newnoargs_e258087cd0daa0ea = function(arg0, arg1) {
        const ret = new Function(getStringFromWasm0(arg0, arg1));
        return addHeapObject(ret);
    };
    imports.wbg.__wbg_new0_7d84e5b2cd9fdc73 = function() {
        const ret = new Date();
        return addHeapObject(ret);
    };
    imports.wbg.__wbg_getTime_2bc4375165f02d15 = function(arg0) {
        const ret = getObject(arg0).getTime();
        return ret;
    };
    imports.wbg.__wbg_self_ce0dbfc45cf2f5be = function() { return handleError(function () {
        const ret = self.self;
        return addHeapObject(ret);
    }, arguments) };
    imports.wbg.__wbg_window_c6fb939a7f436783 = function() { return handleError(function () {
        const ret = window.window;
        return addHeapObject(ret);
    }, arguments) };
    imports.wbg.__wbg_globalThis_d1e6af4856ba331b = function() { return handleError(function () {
        const ret = globalThis.globalThis;
        return addHeapObject(ret);
    }, arguments) };
    imports.wbg.__wbg_global_207b558942527489 = function() { return handleError(function () {
        const ret = global.global;
        return addHeapObject(ret);
    }, arguments) };
    imports.wbg.__wbindgen_is_undefined = function(arg0) {
        const ret = getObject(arg0) === undefined;
        return ret;
    };
    imports.wbg.__wbg_call_27c0f87801dedf93 = function() { return handleError(function (arg0, arg1) {
        const ret = getObject(arg0).call(getObject(arg1));
        return addHeapObject(ret);
    }, arguments) };
    imports.wbg.__wbindgen_object_clone_ref = function(arg0) {
        const ret = getObject(arg0);
        return addHeapObject(ret);
    };
    imports.wbg.__wbindgen_throw = function(arg0, arg1) {
        throw new Error(getStringFromWasm0(arg0, arg1));
    };

    return imports;
}

function __wbg_init_memory(imports, maybe_memory) {

}

function __wbg_finalize_init(instance, module) {
    wasm = instance.exports;
    __wbg_init.__wbindgen_wasm_module = module;
    cachedInt32Memory0 = null;
    cachedUint32Memory0 = null;
    cachedUint8Memory0 = null;

    wasm.__wbindgen_start();
    return wasm;
}

function initSync(module) {
    if (wasm !== undefined) return wasm;

    const imports = __wbg_get_imports();

    __wbg_init_memory(imports);

    if (!(module instanceof WebAssembly.Module)) {
        module = new WebAssembly.Module(module);
    }

    const instance = new WebAssembly.Instance(module, imports);

    return __wbg_finalize_init(instance, module);
}

async function __wbg_init(input) {
    if (wasm !== undefined) return wasm;

    if (typeof input === 'undefined') {
        input = new URL('obot_converter_bg.wasm', import.meta.url);
    }
    const imports = __wbg_get_imports();

    if (typeof input === 'string' || (typeof Request === 'function' && input instanceof Request) || (typeof URL === 'function' && input instanceof URL)) {
        input = fetch(input);
    }

    __wbg_init_memory(imports);

    const { instance, module } = await __wbg_load(await input, imports);

    return __wbg_finalize_init(instance, module);
}

export { initSync }
export default __wbg_init;
