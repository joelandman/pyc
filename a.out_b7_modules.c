#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>


typedef struct {
    const char* name;
    void* (*entry)(void);
} pyc_module_entry;

static pyc_module_entry pyc_modules[] = {
    {NULL, NULL}
};

extern const char* PyStr_AsUTF8(void* obj);
void pyc_run_module(void* moduleNameObj) {
    const char* moduleName = PyStr_AsUTF8(moduleNameObj);
    if (!moduleName) return;
    for (int i = 0; pyc_modules[i].name != NULL; i++) {
        if (strcmp(pyc_modules[i].name, moduleName) == 0) {
            void* result = pyc_modules[i].entry();
            return;
        }
    }
    // Module not found - silently skip (unsupported module)
}

// Stub: sys module - provides argv and stderr
void __module__sys(void) {
    // sys.argv is set up by pyc_setup_sys in MainWrapper.cpp
    // sys.stderr is a file object - we use the real stderr
}

// Stub: os module - provides path operations
void __module__os(void) {
    // os.environ - return an empty dict (no env var support)
    // os.path.exists, isfile, isdir - use real POSIX functions
}

// Stub: subprocess module - provides call and check_output
void __module__subprocess(void) {
    // subprocess.call - use real system()
    // subprocess.check_output - use real popen()
}
