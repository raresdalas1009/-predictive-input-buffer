// The converter was built for a browser page: it checks for a window and a document once.
globalThis.Window = globalThis.Window || Object;
globalThis.window = globalThis.window || globalThis;
globalThis.document = globalThis.document || { body: {} };
