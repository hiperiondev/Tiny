#ifndef TINY_H
#define TINY_H

#include <stdio.h>
#include <stdbool.h>

#ifndef TINY_THREAD_STACK_SIZE
#define TINY_THREAD_STACK_SIZE  128
#endif

#ifndef TINY_THREAD_INDIR_SIZE
#define TINY_THREAD_INDIR_SIZE  256
#endif

typedef struct Tiny_Object Tiny_Object;
typedef struct Tiny_State Tiny_State;

struct Tiny_Value;

// Stores properties about a native
// object. This should be statically allocated
// and only one should exist for each type of
// Native value.
typedef struct
{
	const char* name;

	void(*protectFromGC)(void*);
	void(*free)(void*);
	struct Tiny_Value (*toString)(void*);
} Tiny_NativeProp;

typedef enum
{
	TINY_VAL_NULL,
	TINY_VAL_BOOL,
	TINY_VAL_NUM,
	TINY_VAL_STRING,
	TINY_VAL_CONST_STRING,
	TINY_VAL_NATIVE,
    TINY_VAL_LIGHT_NATIVE
} Tiny_ValueType;

typedef struct Tiny_Value
{
	Tiny_ValueType type;

	union
	{
		bool boolean;
		double number;
		const char* cstr;   // for TINY_VAL_CONST_STRING
        void* addr;         // for TINY_VAL_LIGHT_NATIVE
		Tiny_Object* obj;
	};
} Tiny_Value;

typedef struct Tiny_StateThread
{
    // Each thread stores a reference
    // to its state
    const Tiny_State* state;

    // The garbage collection and heap is thread-local 
    Tiny_Object* gcHead;
    int numObjects;
    int maxNumObjects;
    
    // Global vars are owned by each thread
    Tiny_Value* globalVars;

    int pc, fp, sp;
    Tiny_Value retVal;

    Tiny_Value stack[TINY_THREAD_STACK_SIZE];

    int indirStack[TINY_THREAD_INDIR_SIZE];
    int indirStackSize;

	// Userdata pointer. Set to NULL when InitThread is called. Use it for whatever you want
	void* userdata;
} Tiny_StateThread;

typedef Tiny_Value (*Tiny_ForeignFunction)(Tiny_StateThread* thread, const Tiny_Value* args, int count);

extern const Tiny_Value Tiny_Null;

void* emalloc(size_t size);
void* erealloc(void* mem, size_t newSize);
char* estrdup(const char* string);

void Tiny_ProtectFromGC(Tiny_Value value);

Tiny_Value Tiny_NewBool(bool value);
Tiny_Value Tiny_NewNumber(double value);
Tiny_Value Tiny_NewConstString(const char* string);
Tiny_Value Tiny_NewLightNative(void* ptr);
Tiny_Value Tiny_NewString(Tiny_StateThread* thread, char* string);
Tiny_Value Tiny_NewNative(Tiny_StateThread* thread, void* ptr, const Tiny_NativeProp* prop);

#define Tiny_IsNull(value) (value.type == TINY_VAL_NULL)

inline bool Tiny_ToBool(const Tiny_Value value)
{
    if(value.type != TINY_VAL_BOOL) return false;
    return value.boolean;
}

inline double Tiny_ToNumber(const Tiny_Value value)
{
    if(value.type != TINY_VAL_NUM) return 0;
    return value.number;
}

// Returns NULL if the value isn't a string/const string
const char* Tiny_ToString(const Tiny_Value value);

// Returns value.addr if its a LIGHT_NATIVE
// Returns the normal native address otherwise
void* Tiny_ToAddr(const Tiny_Value value);

// This returns NULL if the value is a LIGHT_NATIVE instead of a NATIVE
// It would also return NULL if the NativeProp supplied when the object was created was NULL,
// either way, you have no information, so deal with it.
const Tiny_NativeProp* Tiny_GetProp(const Tiny_Value value);

Tiny_State* Tiny_CreateState(void);

void Tiny_BindFunction(Tiny_State* state, const char* name, Tiny_ForeignFunction func);
void Tiny_BindConstNumber(Tiny_State* state, const char* name, double value);
void Tiny_BindConstString(Tiny_State* state, const char* name, const char* value);

void Tiny_CompileString(Tiny_State* state, const char* name, const char* string);
void Tiny_CompileFile(Tiny_State* state, const char* filename);

void Tiny_DeleteState(Tiny_State* state);

void Tiny_InitThread(Tiny_StateThread* thread, const Tiny_State* state);

// Sets the PC of the thread to the entry point of the program
// and allocates space for global variables if they're not already
// allocated
// Requires that state is compiled
void Tiny_StartThread(Tiny_StateThread* thread);

// Returns -1 if the global doesn't exist
// Do note that this will return -1 for global constants as well (those are inlined wherever they are used, so they don't really exist)
int Tiny_GetGlobalIndex(const Tiny_State* state, const char* name);

// Returns -1 if the function doesn't exist
int Tiny_GetFunctionIndex(const Tiny_State* state, const char* name);

// Returns the thread->globalVars[globalIndex] (asserts if index < 0)
// You must have started the thread or called Tiny_CallFunction for this to work
// because otherwise, the thread's global variables might not be allocated.
// Don't worry, this will assert that they have.
Tiny_Value Tiny_GetGlobal(const Tiny_StateThread* thread, int globalIndex);

// Sets a global variable at the given index to the given value (asserts if index < 0)
// You must have started the thread or called Tiny_CallFunction for this to work
// because otherwise, the thread's global variables might not be allocated.
// Don't worry, this will assert that they have.
void Tiny_SetGlobal(Tiny_StateThread* thread, int globalIndex, Tiny_Value value);

// Runs the thread until the function exits and returns the retVal.
// functionIndex can be retrieved using Tiny_GetFunctionIndex.
// The only requirement is that the thread has been initialized.
// You can even call this from a foreign function. It keeps track of the
// state of the thread prior to the function call and restores it afterwards.
// This also allocates globals if the thread hasn't been started already, and in that case, once
// the function call is over, the thread will be "done".
Tiny_Value Tiny_CallFunction(Tiny_StateThread* thread, int functionIndex, const Tiny_Value* args, int count);

inline bool Tiny_IsThreadDone(const Tiny_StateThread* thread)
{
    return thread->pc < 0;
}

// Run a single cycle of the thread.
// Could potentially trigger garbage collection
// at the end of the cycle.
// Returns whether the cycle was executed or not.
bool Tiny_ExecuteCycle(Tiny_StateThread* thread);

void Tiny_BindStandardLibrary(Tiny_State* state);

void Tiny_DestroyThread(Tiny_StateThread* thread);

#endif
 
