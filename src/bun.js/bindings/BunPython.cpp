#include "BunPython.h"
#include "JSPyObject.h"
#include "ZigGlobalObject.h"
#include "BunClientData.h"
#include <JavaScriptCore/ObjectConstructor.h>
#include <JavaScriptCore/FunctionPrototype.h>
#include <JavaScriptCore/JSFunction.h>
#include <JavaScriptCore/JSModuleLoader.h>
#include <JavaScriptCore/Completion.h>
#include <JavaScriptCore/JSInternalPromise.h>
#include <wtf/text/WTFString.h>
#include <wtf/text/StringBuilder.h>
#include <memory>
#include <unistd.h>

extern "C" void Bun__atexit(void (*callback)());

namespace Bun {

using namespace JSC;

static std::once_flag pythonInitFlag;

static void finalizePython()
{
    if (Py_IsInitialized()) {
        Py_Finalize();
    }
}

// Forward declarations
static void registerJSImportHook();
static void initPyJSValueType();

void ensurePythonInitialized()
{
    std::call_once(pythonInitFlag, []() {
        if (!Py_IsInitialized()) {
            PyConfig config;
            PyConfig_InitPythonConfig(&config);
            PyConfig_SetString(&config, &config.home, L"/Users/dylan/clones/cpython");
            PyConfig_SetString(&config, &config.stdlib_dir, L"/Users/dylan/clones/cpython/Lib");
            // Disable buffered stdio so Python's print() flushes immediately
            config.buffered_stdio = 0;

            PyStatus status = Py_InitializeFromConfig(&config);
            if (PyStatus_Exception(status)) {
                PyConfig_Clear(&config);
                Py_Initialize();
            } else {
                PyConfig_Clear(&config);
            }

            Bun__atexit(finalizePython);

            // Initialize the PyJSValue type for wrapping JS values in Python
            initPyJSValueType();

            // Register the JS import hook so Python can import JS modules
            registerJSImportHook();
        }
    });
}

// The Python object that wraps a JSValue
struct PyJSValueObject {
    PyObject_HEAD JSValue jsValue;
    JSGlobalObject* globalObject;
};

// Forward declarations
static void pyjsvalue_dealloc(PyObject* self);
static PyObject* pyjsvalue_repr(PyObject* self);
static PyObject* pyjsvalue_getattro(PyObject* self, PyObject* name);
static int pyjsvalue_setattro(PyObject* self, PyObject* name, PyObject* value);
static PyObject* pyjsvalue_call(PyObject* self, PyObject* args, PyObject* kwargs);
static PyObject* pyjsvalue_subscript(PyObject* self, PyObject* key);
static int pyjsvalue_ass_subscript(PyObject* self, PyObject* key, PyObject* value);

// Mapping methods for subscript access (obj[key])
static PyMappingMethods PyJSValue_as_mapping = {
    nullptr, // mp_length
    pyjsvalue_subscript, // mp_subscript
    pyjsvalue_ass_subscript, // mp_ass_subscript
};

// The PyJSValue type object
static PyTypeObject PyJSValue_Type = {
    PyVarObject_HEAD_INIT(NULL, 0) "bun.JSValue", // tp_name
    sizeof(PyJSValueObject), // tp_basicsize
    0, // tp_itemsize
    pyjsvalue_dealloc, // tp_dealloc
    0, // tp_vectorcall_offset
    nullptr, // tp_getattr
    nullptr, // tp_setattr
    nullptr, // tp_as_async
    pyjsvalue_repr, // tp_repr
    nullptr, // tp_as_number
    nullptr, // tp_as_sequence
    &PyJSValue_as_mapping, // tp_as_mapping
    nullptr, // tp_hash
    pyjsvalue_call, // tp_call
    nullptr, // tp_str
    pyjsvalue_getattro, // tp_getattro
    pyjsvalue_setattro, // tp_setattro
    nullptr, // tp_as_buffer
    Py_TPFLAGS_DEFAULT, // tp_flags
    "JavaScript value wrapper", // tp_doc
};

static void initPyJSValueType()
{
    if (PyType_Ready(&PyJSValue_Type) < 0) {
        PyErr_Print();
    }
}

// Create a PyJSValue from a JSValue
PyObject* wrapJSValueForPython(JSGlobalObject* globalObject, JSValue value)
{
    // Convert primitives directly to Python types
    if (value.isUndefined() || value.isNull()) {
        Py_RETURN_NONE;
    }
    if (value.isBoolean()) {
        if (value.asBoolean()) {
            Py_RETURN_TRUE;
        }
        Py_RETURN_FALSE;
    }
    if (value.isInt32()) {
        return PyLong_FromLong(value.asInt32());
    }
    if (value.isNumber()) {
        return PyFloat_FromDouble(value.asNumber());
    }
    if (value.isString()) {
        auto str = value.toWTFString(globalObject);
        auto utf8 = str.utf8();
        return PyUnicode_FromStringAndSize(utf8.data(), utf8.length());
    }

    // For objects, functions, arrays - wrap in PyJSValue
    PyJSValueObject* wrapper = PyObject_New(PyJSValueObject, &PyJSValue_Type);
    if (!wrapper) {
        return nullptr;
    }

    wrapper->jsValue = value;
    wrapper->globalObject = globalObject;

    // Protect from JavaScript GC while Python holds a reference
    if (value.isCell()) {
        gcProtect(value.asCell());
    }

    return reinterpret_cast<PyObject*>(wrapper);
}

// Convert Python value to JSValue
JSValue unwrapPyObjectToJS(JSGlobalObject* globalObject, PyObject* obj)
{
    if (!obj || obj == Py_None) {
        return jsNull();
    }

    // Check if it's a wrapped JSValue - return the original
    if (Py_TYPE(obj) == &PyJSValue_Type) {
        return reinterpret_cast<PyJSValueObject*>(obj)->jsValue;
    }

    // Convert Python primitives to JS primitives
    if (PyBool_Check(obj)) {
        return jsBoolean(obj == Py_True);
    }
    if (PyLong_Check(obj)) {
        return jsNumber(PyLong_AsDouble(obj));
    }
    if (PyFloat_Check(obj)) {
        return jsNumber(PyFloat_AsDouble(obj));
    }
    if (PyUnicode_Check(obj)) {
        const char* str = PyUnicode_AsUTF8(obj);
        if (!str) {
            PyErr_Clear();
            return jsUndefined();
        }
        return jsString(globalObject->vm(), WTF::String::fromUTF8(str));
    }

    // For other Python objects, wrap in JSPyObject
    Py_INCREF(obj);
    auto* zigGlobalObject = jsCast<Zig::GlobalObject*>(globalObject);
    Structure* structure = zigGlobalObject->m_JSPyObjectStructure.get();
    if (!structure) {
        structure = JSPyObject::createStructure(globalObject->vm(), globalObject, globalObject->objectPrototype());
        zigGlobalObject->m_JSPyObjectStructure.set(globalObject->vm(), zigGlobalObject, structure);
    }
    return JSPyObject::create(globalObject->vm(), globalObject, structure, obj);
}

static void pyjsvalue_dealloc(PyObject* self)
{
    PyJSValueObject* wrapper = reinterpret_cast<PyJSValueObject*>(self);

    // Unprotect the JSValue so JavaScript GC can collect it
    if (wrapper->jsValue.isCell()) {
        gcUnprotect(wrapper->jsValue.asCell());
    }

    Py_TYPE(self)->tp_free(self);
}

static PyObject* pyjsvalue_repr(PyObject* self)
{
    PyJSValueObject* wrapper = reinterpret_cast<PyJSValueObject*>(self);
    JSGlobalObject* globalObject = wrapper->globalObject;

    if (!globalObject) {
        return PyUnicode_FromString("<JSValue: no global>");
    }

    VM& vm = globalObject->vm();
    auto scope = DECLARE_CATCH_SCOPE(vm);

    auto str = wrapper->jsValue.toWTFString(globalObject);
    if (scope.exception()) {
        scope.clearException();
        return PyUnicode_FromString("<JSValue>");
    }

    auto utf8 = str.utf8();
    return PyUnicode_FromStringAndSize(utf8.data(), utf8.length());
}

static PyObject* pyjsvalue_getattro(PyObject* self, PyObject* name)
{
    PyJSValueObject* wrapper = reinterpret_cast<PyJSValueObject*>(self);
    JSGlobalObject* globalObject = wrapper->globalObject;

    if (!PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError, "attribute name must be string");
        return nullptr;
    }

    const char* attrName = PyUnicode_AsUTF8(name);
    if (!attrName) {
        return nullptr;
    }

    if (!globalObject) {
        PyErr_SetString(PyExc_RuntimeError, "JavaScript global not available");
        return nullptr;
    }

    VM& vm = globalObject->vm();
    auto scope = DECLARE_CATCH_SCOPE(vm);

    JSObject* jsObj = wrapper->jsValue.getObject();
    if (!jsObj) {
        PyErr_SetString(PyExc_TypeError, "JavaScript value is not an object");
        return nullptr;
    }

    Identifier ident = Identifier::fromString(vm, WTF::String::fromUTF8(attrName));
    JSValue result = jsObj->get(globalObject, ident);

    if (scope.exception()) {
        scope.clearException();
        PyErr_Format(PyExc_AttributeError, "Error accessing '%s'", attrName);
        return nullptr;
    }

    return wrapJSValueForPython(globalObject, result);
}

static int pyjsvalue_setattro(PyObject* self, PyObject* name, PyObject* value)
{
    PyJSValueObject* wrapper = reinterpret_cast<PyJSValueObject*>(self);
    JSGlobalObject* globalObject = wrapper->globalObject;

    if (!PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError, "attribute name must be string");
        return -1;
    }

    const char* attrName = PyUnicode_AsUTF8(name);
    if (!attrName) {
        return -1;
    }

    if (!globalObject) {
        PyErr_SetString(PyExc_RuntimeError, "JavaScript global not available");
        return -1;
    }

    VM& vm = globalObject->vm();
    auto scope = DECLARE_CATCH_SCOPE(vm);

    JSObject* jsObj = wrapper->jsValue.getObject();
    if (!jsObj) {
        PyErr_SetString(PyExc_TypeError, "JavaScript value is not an object");
        return -1;
    }

    Identifier ident = Identifier::fromString(vm, WTF::String::fromUTF8(attrName));
    JSValue jsVal = unwrapPyObjectToJS(globalObject, value);

    jsObj->putDirect(vm, ident, jsVal);

    if (scope.exception()) {
        scope.clearException();
        PyErr_Format(PyExc_AttributeError, "Error setting '%s'", attrName);
        return -1;
    }

    return 0;
}

static PyObject* pyjsvalue_call(PyObject* self, PyObject* args, PyObject* kwargs)
{
    PyJSValueObject* wrapper = reinterpret_cast<PyJSValueObject*>(self);
    JSGlobalObject* globalObject = wrapper->globalObject;

    if (!globalObject) {
        PyErr_SetString(PyExc_RuntimeError, "JavaScript global not available");
        return nullptr;
    }

    VM& vm = globalObject->vm();
    auto scope = DECLARE_CATCH_SCOPE(vm);

    JSValue calleeValue = wrapper->jsValue;

    // Check if it's callable
    auto callData = JSC::getCallData(calleeValue);
    if (callData.type == CallData::Type::None) {
        PyErr_SetString(PyExc_TypeError, "JavaScript value is not callable");
        return nullptr;
    }

    // Convert Python args to JS args
    Py_ssize_t argc = PyTuple_Size(args);
    MarkedArgumentBuffer jsArgs;

    for (Py_ssize_t i = 0; i < argc; i++) {
        PyObject* arg = PyTuple_GetItem(args, i); // Borrowed reference
        jsArgs.append(unwrapPyObjectToJS(globalObject, arg));
    }

    // Call the function
    JSValue result = JSC::profiledCall(globalObject, ProfilingReason::API, calleeValue, callData, jsUndefined(), jsArgs);

    if (scope.exception()) {
        JSValue exception = scope.exception()->value();
        scope.clearException();

        // Try to get exception message
        if (exception.isObject()) {
            JSObject* errObj = exception.getObject();
            JSValue msgVal = errObj->get(globalObject, Identifier::fromString(vm, "message"_s));
            if (msgVal.isString()) {
                auto msg = msgVal.toWTFString(globalObject);
                PyErr_Format(PyExc_RuntimeError, "JavaScript error: %s", msg.utf8().data());
                return nullptr;
            }
        }
        PyErr_SetString(PyExc_RuntimeError, "JavaScript error during call");
        return nullptr;
    }

    return wrapJSValueForPython(globalObject, result);
}

// Subscript access: obj[key]
static PyObject* pyjsvalue_subscript(PyObject* self, PyObject* key)
{
    PyJSValueObject* wrapper = reinterpret_cast<PyJSValueObject*>(self);
    JSGlobalObject* globalObject = wrapper->globalObject;

    if (!globalObject) {
        PyErr_SetString(PyExc_RuntimeError, "JavaScript global not available");
        return nullptr;
    }

    VM& vm = globalObject->vm();
    auto scope = DECLARE_CATCH_SCOPE(vm);

    JSObject* jsObj = wrapper->jsValue.getObject();
    if (!jsObj) {
        PyErr_SetString(PyExc_TypeError, "JavaScript value is not an object");
        return nullptr;
    }

    JSValue result;

    // Handle integer keys for array access
    if (PyLong_Check(key)) {
        long index = PyLong_AsLong(key);
        if (index >= 0) {
            result = jsObj->get(globalObject, static_cast<unsigned>(index));
        } else {
            PyErr_SetString(PyExc_IndexError, "negative index not supported");
            return nullptr;
        }
    } else if (PyUnicode_Check(key)) {
        const char* keyStr = PyUnicode_AsUTF8(key);
        if (!keyStr) {
            return nullptr;
        }
        Identifier ident = Identifier::fromString(vm, WTF::String::fromUTF8(keyStr));
        result = jsObj->get(globalObject, ident);
    } else {
        PyErr_SetString(PyExc_TypeError, "key must be string or integer");
        return nullptr;
    }

    if (scope.exception()) {
        scope.clearException();
        PyErr_SetString(PyExc_KeyError, "Error accessing property");
        return nullptr;
    }

    return wrapJSValueForPython(globalObject, result);
}

// Subscript assignment: obj[key] = value
static int pyjsvalue_ass_subscript(PyObject* self, PyObject* key, PyObject* value)
{
    PyJSValueObject* wrapper = reinterpret_cast<PyJSValueObject*>(self);
    JSGlobalObject* globalObject = wrapper->globalObject;

    if (!globalObject) {
        PyErr_SetString(PyExc_RuntimeError, "JavaScript global not available");
        return -1;
    }

    VM& vm = globalObject->vm();
    auto scope = DECLARE_CATCH_SCOPE(vm);

    JSObject* jsObj = wrapper->jsValue.getObject();
    if (!jsObj) {
        PyErr_SetString(PyExc_TypeError, "JavaScript value is not an object");
        return -1;
    }

    JSValue jsVal = unwrapPyObjectToJS(globalObject, value);

    // Handle integer keys for array access
    if (PyLong_Check(key)) {
        long index = PyLong_AsLong(key);
        if (index >= 0) {
            jsObj->putDirectIndex(globalObject, static_cast<unsigned>(index), jsVal);
        } else {
            PyErr_SetString(PyExc_IndexError, "negative index not supported");
            return -1;
        }
    } else if (PyUnicode_Check(key)) {
        const char* keyStr = PyUnicode_AsUTF8(key);
        if (!keyStr) {
            return -1;
        }
        Identifier ident = Identifier::fromString(vm, WTF::String::fromUTF8(keyStr));
        jsObj->putDirect(vm, ident, jsVal);
    } else {
        PyErr_SetString(PyExc_TypeError, "key must be string or integer");
        return -1;
    }

    if (scope.exception()) {
        scope.clearException();
        PyErr_SetString(PyExc_KeyError, "Error setting property");
        return -1;
    }

    return 0;
}

static const char* BUN_GLOBAL_KEY = "bun.jsglobal";

// Store JSGlobalObject in Python's thread state dict
static void setThreadJSGlobal(JSGlobalObject* global)
{
    PyObject* threadDict = PyThreadState_GetDict();
    if (!threadDict)
        return;

    PyObject* capsule = PyCapsule_New(global, BUN_GLOBAL_KEY, nullptr);
    if (capsule) {
        PyDict_SetItemString(threadDict, BUN_GLOBAL_KEY, capsule);
        Py_DECREF(capsule);
    }
}

// Retrieve JSGlobalObject from Python's thread state dict
static JSGlobalObject* getThreadJSGlobal()
{
    PyObject* threadDict = PyThreadState_GetDict();
    if (!threadDict)
        return nullptr;

    PyObject* capsule = PyDict_GetItemString(threadDict, BUN_GLOBAL_KEY);
    if (!capsule || !PyCapsule_CheckExact(capsule))
        return nullptr;

    return static_cast<JSGlobalObject*>(PyCapsule_GetPointer(capsule, BUN_GLOBAL_KEY));
}

// C function callable from Python to load a JS/TS/JSX module
static PyObject* bun_load_js_module(PyObject* self, PyObject* args)
{
    const char* filePath;

    if (!PyArg_ParseTuple(args, "s", &filePath)) {
        return nullptr;
    }

    JSGlobalObject* globalObject = getThreadJSGlobal();
    if (!globalObject) {
        PyErr_SetString(PyExc_RuntimeError, "No JavaScript context available");
        return nullptr;
    }

    VM& vm = globalObject->vm();
    auto scope = DECLARE_CATCH_SCOPE(vm);

    // Create the file URL for the module
    WTF::String filePathStr = WTF::String::fromUTF8(filePath);

    // Use importModule to load the ES module
    auto* promise = JSC::importModule(globalObject, JSC::Identifier::fromString(vm, filePathStr), jsUndefined(), jsUndefined(), jsUndefined());

    if (!promise) {
        if (scope.exception()) {
            JSValue exception = scope.exception()->value();
            scope.clearException();
            auto msg = exception.toWTFString(globalObject);
            PyErr_Format(PyExc_RuntimeError, "JavaScript error: %s", msg.utf8().data());
        } else {
            PyErr_Format(PyExc_RuntimeError, "Failed to import module: %s", filePath);
        }
        return nullptr;
    }

    // Drain the microtask queue to allow the module to load
    vm.drainMicrotasks();

    auto status = promise->status(vm);

    if (status == JSC::JSPromise::Status::Fulfilled) {
        JSValue result = promise->result(vm);
        return wrapJSValueForPython(globalObject, result);
    } else if (status == JSC::JSPromise::Status::Rejected) {
        JSValue error = promise->result(vm);
        auto msg = error.toWTFString(globalObject);
        PyErr_Format(PyExc_RuntimeError, "JavaScript error: %s", msg.utf8().data());
        return nullptr;
    } else {
        // Promise is still pending - this shouldn't happen for simple modules
        PyErr_SetString(PyExc_RuntimeError, "Module loading is pending - async imports not yet supported");
        return nullptr;
    }
}

// Get the current working directory
static PyObject* bun_get_cwd(PyObject* self, PyObject* args)
{
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        return PyUnicode_FromString(cwd);
    }
    Py_RETURN_NONE;
}

static PyMethodDef bunModuleMethods[] = {
    { "_load_js_module", bun_load_js_module, METH_VARARGS, "Load a JavaScript module" },
    { "_get_cwd", bun_get_cwd, METH_NOARGS, "Get current working directory" },
    { nullptr, nullptr, 0, nullptr }
};

static struct PyModuleDef bunModuleDef = {
    PyModuleDef_HEAD_INIT,
    "_bun",
    "Bun internal module",
    -1,
    bunModuleMethods
};

// Python code for the JS import hook
static const char* jsImportHookCode = R"(
import sys
import os

class JSModuleFinder:
    def find_spec(self, fullname, path, target=None):
        import _bun

        # Get the directory to search from caller's __file__ or cwd
        frame = sys._getframe(1)
        while frame:
            if '__file__' in frame.f_globals:
                base_dir = os.path.dirname(os.path.abspath(frame.f_globals['__file__']))
                break
            frame = frame.f_back
        else:
            base_dir = _bun._get_cwd() or os.getcwd()

        # Look for JS/TS/JSX/TSX files
        for ext in ['.js', '.ts', '.jsx', '.tsx', '.mjs', '.mts']:
            js_path = os.path.join(base_dir, fullname + ext)
            if os.path.exists(js_path):
                from importlib.machinery import ModuleSpec
                return ModuleSpec(fullname, JSModuleLoader(js_path), origin=js_path)

        return None


class JSModuleLoader:
    def __init__(self, path):
        self.path = path

    def create_module(self, spec):
        import _bun
        return _bun._load_js_module(self.path)

    def exec_module(self, module):
        pass


sys.meta_path.insert(0, JSModuleFinder())
)";

static bool jsImportHookRegistered = false;

static void registerJSImportHook()
{
    if (jsImportHookRegistered)
        return;

    // Create the _bun module
    PyObject* bunModule = PyModule_Create(&bunModuleDef);
    if (!bunModule) {
        PyErr_Print();
        return;
    }
    PyObject* sysModules = PyImport_GetModuleDict();
    PyDict_SetItemString(sysModules, "_bun", bunModule);
    Py_DECREF(bunModule);

    // Execute the import hook registration code
    PyObject* mainModule = PyImport_AddModule("__main__");
    PyObject* mainDict = PyModule_GetDict(mainModule);

    PyObject* result = PyRun_String(jsImportHookCode, Py_file_input, mainDict, mainDict);
    if (!result) {
        PyErr_Print();
        return;
    }
    Py_DECREF(result);

    jsImportHookRegistered = true;
}

JSC::SyntheticSourceProvider::SyntheticSourceGenerator
generatePythonModuleSourceCode(JSC::JSGlobalObject* globalObject, const WTF::String& filePath)
{
    return [filePath = filePath.isolatedCopy()](JSC::JSGlobalObject* lexicalGlobalObject,
               JSC::Identifier moduleKey,
               Vector<JSC::Identifier, 4>& exportNames,
               JSC::MarkedArgumentBuffer& exportValues) -> void {
        VM& vm = lexicalGlobalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);

        ensurePythonInitialized();

        // Set the JavaScript global for this thread so Python can import JS modules
        setThreadJSGlobal(lexicalGlobalObject);

        // Read the Python file
        auto pathUTF8 = filePath.utf8();
        FILE* fp = fopen(pathUTF8.data(), "rb");
        if (!fp) {
            throwTypeError(lexicalGlobalObject, scope, makeString("Cannot open Python file: "_s, filePath));
            return;
        }

        // Read file content
        fseek(fp, 0, SEEK_END);
        long fileSize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        auto fileContent = std::make_unique<char[]>(fileSize + 1);
        size_t bytesRead = fread(fileContent.get(), 1, fileSize, fp);
        fclose(fp);
        fileContent[bytesRead] = '\0';

        // Create a unique module name based on the file path
        StringBuilder moduleNameBuilder;
        for (unsigned i = 0; i < filePath.length(); i++) {
            UChar c = filePath[i];
            if (c == '/' || c == '.' || c == '-' || c == '\\')
                moduleNameBuilder.append('_');
            else
                moduleNameBuilder.append(c);
        }
        WTF::String moduleName = moduleNameBuilder.toString();
        auto moduleNameUTF8 = moduleName.utf8();

        // Compile the Python source
        PyObject* code = Py_CompileString(fileContent.get(), pathUTF8.data(), Py_file_input);

        if (!code) {
            PyErr_Print();
            PyErr_Clear();
            throwTypeError(lexicalGlobalObject, scope, makeString("Python compile error in: "_s, filePath));
            return;
        }

        // Execute as a module
        PyObject* module = PyImport_ExecCodeModule(moduleNameUTF8.data(), code);
        Py_DECREF(code);

        if (!module) {
            PyErr_Print();
            PyErr_Clear();
            throwTypeError(lexicalGlobalObject, scope, makeString("Python execution error in: "_s, filePath));
            return;
        }

        // Get module dict (borrowed reference)
        PyObject* dict = PyModule_GetDict(module);

        // Create the module object as default export
        auto* zigGlobalObject = jsCast<Zig::GlobalObject*>(lexicalGlobalObject);
        Structure* structure = zigGlobalObject->m_JSPyObjectStructure.get();
        if (!structure) {
            structure = JSPyObject::createStructure(vm, lexicalGlobalObject, lexicalGlobalObject->objectPrototype());
            zigGlobalObject->m_JSPyObjectStructure.set(vm, zigGlobalObject, structure);
        }

        // Add default export - the module itself
        exportNames.append(vm.propertyNames->defaultKeyword);
        JSPyObject* moduleValue = JSPyObject::create(vm, lexicalGlobalObject, structure, module);
        exportValues.append(moduleValue);

        // Iterate module dict and add named exports for public symbols
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(dict, &pos, &key, &value)) {
            if (!PyUnicode_Check(key))
                continue;

            const char* keyStr = PyUnicode_AsUTF8(key);
            if (!keyStr || keyStr[0] == '_')
                continue; // Skip private/dunder

            exportNames.append(Identifier::fromString(vm, String::fromUTF8(keyStr)));
            exportValues.append(JSPyObject::pyObjectToJSValue(lexicalGlobalObject, value));
        }

        // Don't DECREF module here - the JSPyObject holds a reference
    };
}

} // namespace Bun
