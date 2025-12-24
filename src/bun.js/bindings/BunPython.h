#pragma once

#include "root.h"
#include <JavaScriptCore/SyntheticModuleRecord.h>
#include <Python.h>

namespace JSC {
class JSGlobalObject;
}

namespace Bun {

// Ensure Python interpreter is initialized
void ensurePythonInitialized();

// Generate module source code for importing Python files as ES modules
JSC::SyntheticSourceProvider::SyntheticSourceGenerator
generatePythonModuleSourceCode(JSC::JSGlobalObject* globalObject, const WTF::String& filePath);

// Wrap a JSValue for use in Python (creates PyJSValue)
PyObject* wrapJSValueForPython(JSC::JSGlobalObject* globalObject, JSC::JSValue value);

// Unwrap a Python object to JSValue (may create JSPyObject for complex types)
JSC::JSValue unwrapPyObjectToJS(JSC::JSGlobalObject* globalObject, PyObject* obj);

} // namespace Bun
