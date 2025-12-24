#include "JSPyObject.h"
#include "BunPython.h"
#include "ZigGlobalObject.h"
#include "BunClientData.h"
#include <JavaScriptCore/ObjectConstructor.h>
#include <JavaScriptCore/FunctionPrototype.h>
#include <JavaScriptCore/JSFunction.h>
#include <wtf/text/WTFString.h>

namespace Bun {

using namespace JSC;

// Forward declaration for toString
static JSC_DECLARE_HOST_FUNCTION(jsPyObjectToString);

// Forward declaration for call
static JSC_DECLARE_HOST_FUNCTION(jsPyObjectCall);

const ClassInfo JSPyObject::s_info = { "PythonValue"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSPyObject) };

template<typename Visitor>
void JSPyObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSPyObject* thisObject = jsCast<JSPyObject*>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
}

DEFINE_VISIT_CHILDREN(JSPyObject);

void JSPyObject::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
}

JSC::GCClient::IsoSubspace* JSPyObject::subspaceForImpl(JSC::VM& vm)
{
    return WebCore::subspaceForImpl<JSPyObject, WebCore::UseCustomHeapCellType::No>(
        vm,
        [](auto& spaces) { return spaces.m_clientSubspaceForPyObject.get(); },
        [](auto& spaces, auto&& space) { spaces.m_clientSubspaceForPyObject = std::forward<decltype(space)>(space); },
        [](auto& spaces) { return spaces.m_subspaceForPyObject.get(); },
        [](auto& spaces, auto&& space) { spaces.m_subspaceForPyObject = std::forward<decltype(space)>(space); });
}

// Convert PyObject to JSValue - may return JSPyObject for complex types
JSValue JSPyObject::pyObjectToJSValue(JSGlobalObject* globalObject, PyObject* obj)
{
    if (!obj || obj == Py_None) {
        return jsNull();
    }

    VM& vm = globalObject->vm();

    // Primitive types get converted directly
    if (PyBool_Check(obj)) {
        return jsBoolean(obj == Py_True);
    }

    if (PyLong_Check(obj)) {
        // Check if it fits in a safe integer range
        int overflow;
        long long val = PyLong_AsLongLongAndOverflow(obj, &overflow);
        if (overflow == 0) {
            return jsNumber(static_cast<double>(val));
        }
        // For large integers, convert to double (may lose precision)
        return jsNumber(PyLong_AsDouble(obj));
    }

    if (PyFloat_Check(obj)) {
        return jsNumber(PyFloat_AsDouble(obj));
    }

    if (PyUnicode_Check(obj)) {
        Py_ssize_t size;
        const char* str = PyUnicode_AsUTF8AndSize(obj, &size);
        if (str) {
            return jsString(vm, WTF::String::fromUTF8({ str, static_cast<size_t>(size) }));
        }
        return jsNull();
    }

    // For all other types (lists, dicts, objects, callables, etc.),
    // wrap in JSPyObject for lazy access
    auto* zigGlobalObject = jsCast<Zig::GlobalObject*>(globalObject);
    Structure* structure = zigGlobalObject->m_JSPyObjectStructure.get();
    if (!structure) {
        structure = JSPyObject::createStructure(vm, globalObject, globalObject->objectPrototype());
        zigGlobalObject->m_JSPyObjectStructure.set(vm, zigGlobalObject, structure);
    }

    return JSPyObject::create(vm, globalObject, structure, obj);
}

// Property access - proxy to Python's getattr
bool JSPyObject::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    JSPyObject* thisObject = jsCast<JSPyObject*>(object);
    VM& vm = globalObject->vm();

    // Handle special JS properties
    if (propertyName == vm.propertyNames->toStringTagSymbol) {
        slot.setValue(object, static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly), jsString(vm, String("PythonValue"_s)));
        return true;
    }

    // Handle toString
    if (propertyName == vm.propertyNames->toString) {
        slot.setValue(object, static_cast<unsigned>(PropertyAttribute::DontEnum),
            JSFunction::create(vm, globalObject, 0, "toString"_s, jsPyObjectToString, ImplementationVisibility::Public));
        return true;
    }

    // Handle nodejs.util.inspect.custom for console.log
    if (propertyName == Identifier::fromUid(vm.symbolRegistry().symbolForKey("nodejs.util.inspect.custom"_s))) {
        slot.setValue(object, static_cast<unsigned>(PropertyAttribute::DontEnum),
            JSFunction::create(vm, globalObject, 0, "inspect"_s, jsPyObjectToString, ImplementationVisibility::Public));
        return true;
    }

    // Convert property name to Python string
    auto* nameString = propertyName.publicName();
    if (!nameString) {
        return Base::getOwnPropertySlot(object, globalObject, propertyName, slot);
    }

    auto nameUTF8 = nameString->utf8();
    PyObject* pyName = PyUnicode_FromStringAndSize(nameUTF8.data(), nameUTF8.length());
    if (!pyName) {
        PyErr_Clear();
        return false;
    }

    // First try attribute access (for regular objects)
    PyObject* attr = PyObject_GetAttr(thisObject->m_pyObject, pyName);
    if (!attr) {
        PyErr_Clear();
        // If attribute access fails, try item access (for dicts/mappings)
        if (PyMapping_Check(thisObject->m_pyObject)) {
            attr = PyObject_GetItem(thisObject->m_pyObject, pyName);
            if (!attr) {
                PyErr_Clear();
            }
        }
    }
    Py_DECREF(pyName);

    if (!attr) {
        return false;
    }

    JSValue jsAttr = pyObjectToJSValue(globalObject, attr);
    Py_DECREF(attr);

    slot.setValue(object, static_cast<unsigned>(PropertyAttribute::None), jsAttr);
    return true;
}

bool JSPyObject::getOwnPropertySlotByIndex(JSObject* object, JSGlobalObject* globalObject, unsigned index, PropertySlot& slot)
{
    JSPyObject* thisObject = jsCast<JSPyObject*>(object);

    PyObject* item = PySequence_GetItem(thisObject->m_pyObject, static_cast<Py_ssize_t>(index));
    if (!item) {
        PyErr_Clear();
        return false;
    }

    JSValue jsItem = pyObjectToJSValue(globalObject, item);
    Py_DECREF(item);

    slot.setValue(object, static_cast<unsigned>(PropertyAttribute::None), jsItem);
    return true;
}

void JSPyObject::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArray& propertyNames, DontEnumPropertiesMode mode)
{
    JSPyObject* thisObject = jsCast<JSPyObject*>(object);
    VM& vm = globalObject->vm();

    // Get dir() of the object
    PyObject* dir = PyObject_Dir(thisObject->m_pyObject);
    if (!dir) {
        PyErr_Clear();
        return;
    }

    Py_ssize_t len = PyList_Size(dir);
    for (Py_ssize_t i = 0; i < len; i++) {
        PyObject* name = PyList_GetItem(dir, i); // borrowed reference
        if (PyUnicode_Check(name)) {
            const char* nameStr = PyUnicode_AsUTF8(name);
            if (nameStr && nameStr[0] != '_') { // Skip private/dunder
                propertyNames.add(Identifier::fromString(vm, String::fromUTF8(nameStr)));
            }
        }
    }
    Py_DECREF(dir);
}

// Helper to convert JSValue to PyObject
static PyObject* jsValueToPyObject(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isNull() || value.isUndefined()) {
        Py_INCREF(Py_None);
        return Py_None;
    }
    if (value.isBoolean()) {
        PyObject* result = value.asBoolean() ? Py_True : Py_False;
        Py_INCREF(result);
        return result;
    }
    if (value.isNumber()) {
        double num = value.asNumber();
        constexpr double maxSafeInt = 9007199254740992.0;
        if (std::floor(num) == num && num >= -maxSafeInt && num <= maxSafeInt) {
            return PyLong_FromLongLong(static_cast<long long>(num));
        }
        return PyFloat_FromDouble(num);
    }
    if (value.isString()) {
        auto str = value.toWTFString(globalObject);
        auto utf8 = str.utf8();
        return PyUnicode_FromStringAndSize(utf8.data(), utf8.length());
    }
    if (auto* pyVal = jsDynamicCast<JSPyObject*>(value)) {
        PyObject* obj = pyVal->pyObject();
        Py_INCREF(obj);
        return obj;
    }
    // For other JS objects, return None for now
    Py_INCREF(Py_None);
    return Py_None;
}

bool JSPyObject::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    JSPyObject* thisObject = jsCast<JSPyObject*>(cell);

    auto* nameString = propertyName.publicName();
    if (!nameString) {
        return false;
    }

    auto nameUTF8 = nameString->utf8();
    PyObject* pyName = PyUnicode_FromStringAndSize(nameUTF8.data(), nameUTF8.length());
    if (!pyName) {
        PyErr_Clear();
        return false;
    }

    PyObject* pyValue = jsValueToPyObject(globalObject, value);
    if (!pyValue) {
        Py_DECREF(pyName);
        PyErr_Clear();
        return false;
    }

    int result = -1;

    // For dicts/mappings, use item assignment
    if (PyDict_Check(thisObject->m_pyObject)) {
        result = PyDict_SetItem(thisObject->m_pyObject, pyName, pyValue);
    } else if (PyMapping_Check(thisObject->m_pyObject)) {
        result = PyObject_SetItem(thisObject->m_pyObject, pyName, pyValue);
    } else {
        // For other objects, try attribute assignment
        result = PyObject_SetAttr(thisObject->m_pyObject, pyName, pyValue);
    }

    Py_DECREF(pyName);
    Py_DECREF(pyValue);

    if (result < 0) {
        PyErr_Clear();
        return false;
    }

    return true;
}

bool JSPyObject::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool)
{
    JSPyObject* thisObject = jsCast<JSPyObject*>(cell);

    if (!PySequence_Check(thisObject->m_pyObject)) {
        return false;
    }

    PyObject* pyValue = jsValueToPyObject(globalObject, value);
    if (!pyValue) {
        PyErr_Clear();
        return false;
    }

    int result = PySequence_SetItem(thisObject->m_pyObject, static_cast<Py_ssize_t>(index), pyValue);
    Py_DECREF(pyValue);

    if (result < 0) {
        PyErr_Clear();
        return false;
    }

    return true;
}

// toString - returns Python's str() representation
JSC_DEFINE_HOST_FUNCTION(jsPyObjectToString, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue thisValue = callFrame->thisValue();
    JSPyObject* thisObject = jsDynamicCast<JSPyObject*>(thisValue);
    if (!thisObject) {
        return JSValue::encode(jsString(vm, String("[object PythonValue]"_s)));
    }

    PyObject* str = PyObject_Str(thisObject->pyObject());
    if (!str) {
        PyErr_Clear();
        return JSValue::encode(jsString(vm, String("[object PythonValue]"_s)));
    }

    const char* utf8 = PyUnicode_AsUTF8(str);
    if (!utf8) {
        Py_DECREF(str);
        PyErr_Clear();
        return JSValue::encode(jsString(vm, String("[object PythonValue]"_s)));
    }

    JSValue result = jsString(vm, WTF::String::fromUTF8(utf8));
    Py_DECREF(str);
    return JSValue::encode(result);
}

// Call Python function from JS
JSC_DEFINE_HOST_FUNCTION(jsPyObjectCall, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSPyObject* thisObject = jsDynamicCast<JSPyObject*>(callFrame->jsCallee());
    if (!thisObject) {
        throwTypeError(globalObject, scope, "Not a Python callable"_s);
        return {};
    }

    PyObject* pyFunc = thisObject->pyObject();
    if (!PyCallable_Check(pyFunc)) {
        throwTypeError(globalObject, scope, "Python object is not callable"_s);
        return {};
    }

    // Convert JS arguments to Python tuple
    size_t argCount = callFrame->argumentCount();
    PyObject* args = PyTuple_New(static_cast<Py_ssize_t>(argCount));
    if (!args) {
        throwOutOfMemoryError(globalObject, scope);
        return {};
    }

    for (size_t i = 0; i < argCount; i++) {
        JSValue jsArg = callFrame->uncheckedArgument(i);
        PyObject* pyArg = nullptr;

        // Convert JS value to Python
        if (jsArg.isNull() || jsArg.isUndefined()) {
            pyArg = Py_None;
            Py_INCREF(pyArg);
        } else if (jsArg.isBoolean()) {
            pyArg = jsArg.asBoolean() ? Py_True : Py_False;
            Py_INCREF(pyArg);
        } else if (jsArg.isNumber()) {
            double num = jsArg.asNumber();
            constexpr double maxSafeInt = 9007199254740992.0; // 2^53
            if (std::floor(num) == num && num >= -maxSafeInt && num <= maxSafeInt) {
                pyArg = PyLong_FromLongLong(static_cast<long long>(num));
            } else {
                pyArg = PyFloat_FromDouble(num);
            }
        } else if (jsArg.isString()) {
            auto str = jsArg.toWTFString(globalObject);
            RETURN_IF_EXCEPTION(scope, {});
            auto utf8 = str.utf8();
            pyArg = PyUnicode_FromStringAndSize(utf8.data(), utf8.length());
        } else if (auto* pyVal = jsDynamicCast<JSPyObject*>(jsArg)) {
            // Unwrap JSPyObject back to PyObject
            pyArg = pyVal->pyObject();
            Py_INCREF(pyArg);
        } else {
            // For other JS objects, pass as opaque for now
            pyArg = Py_None;
            Py_INCREF(pyArg);
        }

        if (!pyArg) {
            Py_DECREF(args);
            throwTypeError(globalObject, scope, "Failed to convert argument to Python"_s);
            return {};
        }
        PyTuple_SET_ITEM(args, i, pyArg); // steals reference
    }

    // Call the Python function
    PyObject* result = PyObject_CallObject(pyFunc, args);
    Py_DECREF(args);

    if (!result) {
        // Get Python exception info
        PyObject *type, *value, *traceback;
        PyErr_Fetch(&type, &value, &traceback);
        PyErr_NormalizeException(&type, &value, &traceback);

        WTF::String errorMessage = "Python error"_s;
        if (value) {
            PyObject* str = PyObject_Str(value);
            if (str) {
                const char* errStr = PyUnicode_AsUTF8(str);
                if (errStr) {
                    errorMessage = WTF::String::fromUTF8(errStr);
                }
                Py_DECREF(str);
            }
        }

        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(traceback);

        throwTypeError(globalObject, scope, errorMessage);
        return {};
    }

    JSValue jsResult = JSPyObject::pyObjectToJSValue(globalObject, result);
    Py_DECREF(result);

    return JSValue::encode(jsResult);
}

CallData JSPyObject::getCallData(JSCell* cell)
{
    JSPyObject* thisObject = jsCast<JSPyObject*>(cell);

    CallData callData;
    if (thisObject->isCallable()) {
        callData.type = CallData::Type::Native;
        callData.native.function = jsPyObjectCall;
    }
    return callData;
}

} // namespace Bun
