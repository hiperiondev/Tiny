// tiny.c -- an bytecode-based interpreter for the tiny language
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <stdbool.h>

#include "tiny.h"
#include "tiny_detail.h"
#include "stretchy_buffer.h"
#include "t_mem.h"

const Tiny_Value Tiny_Null = { TINY_VAL_NULL };

static int NumNumbers = 0;
static double Numbers[MAX_NUMBERS];

static int NumStrings = 0;
static char Strings[MAX_STRINGS][MAX_TOK_LEN] = { 0 };

#define emalloc(size) malloc(size)
#define erealloc(mem, size) realloc(mem, size)

#if 0

void* emalloc(size_t size)
{
	void* data = malloc(size);
	assert(data && "Out of memory!");
	return data;
}

void* erealloc(void* mem, size_t newSize)
{
	void* newMem = realloc(mem, newSize);
	assert(newMem && "Out of memory!");
	return newMem;
}

#endif

char* estrdup(const char* string)
{
	char* dupString = emalloc(strlen(string) + 1);
	strcpy(dupString, string);
	return dupString;
}

static void DeleteObject(Tiny_Object* obj)
{
	if(obj->type == TINY_VAL_STRING) free(obj->string);
	if (obj->type == TINY_VAL_NATIVE)
	{
		if (obj->nat.prop && obj->nat.prop->finalize)
			obj->nat.prop->finalize(obj->nat.addr);
	}

	free(obj);
}

static inline bool IsObject(Tiny_Value val)
{
	return val.type == TINY_VAL_STRING || val.type == TINY_VAL_NATIVE;
}

void Tiny_ProtectFromGC(Tiny_Value value)
{
    if(!IsObject(value))
        return;

    Tiny_Object* obj = value.obj;

    assert(obj);	
	
	if(obj->marked) return;
	
	if(obj->type == TINY_VAL_NATIVE)
	{
		if(obj->nat.prop && obj->nat.prop->protectFromGC)
			obj->nat.prop->protectFromGC(obj->nat.addr);
	}

	obj->marked = 1;
}

static void MarkAll(Tiny_StateThread* thread);

static void Sweep(Tiny_StateThread* thread)
{
	Tiny_Object** object = &thread->gcHead;
	while(*object)
	{
		if(!(*object)->marked)
		{
			Tiny_Object* unreached = *object;
			--thread->numObjects;
			*object = unreached->next;
			DeleteObject(unreached);
		}
		else
		{
			(*object)->marked = 0;
			object = &(*object)->next;
		}
	}
}

static void GarbageCollect(Tiny_StateThread* thread)
{
	MarkAll(thread);
	Sweep(thread);
	thread->maxNumObjects = thread->numObjects * 2;
}

const char* Tiny_ToString(const Tiny_Value value)
{
	if (value.type == TINY_VAL_CONST_STRING) return value.cstr;
    if(value.type != TINY_VAL_STRING) return NULL;

    return value.obj->string;
}

void* Tiny_ToAddr(const Tiny_Value value)
{
    if(value.type == TINY_VAL_LIGHT_NATIVE) return value.addr;
    if(value.type != TINY_VAL_NATIVE) return NULL;

    return value.obj->nat.addr;
}

const Tiny_NativeProp* Tiny_GetProp(const Tiny_Value value)
{
    if(value.type != TINY_VAL_NATIVE) return NULL;
    return value.obj->nat.prop;
}

static Tiny_Object* NewObject(Tiny_StateThread* thread, Tiny_ValueType type)
{
	Tiny_Object* obj = emalloc(sizeof(Tiny_Object));
	
	obj->type = type;
	obj->next = thread->gcHead;
	thread->gcHead = obj;
	obj->marked = 0;
	
	thread->numObjects++;
	
	return obj;
}

Tiny_Value Tiny_NewLightNative(void* ptr)
{
    Tiny_Value val;

    val.type = TINY_VAL_LIGHT_NATIVE;
    val.addr = ptr;

    return val;
}

Tiny_Value Tiny_NewNative(Tiny_StateThread* thread, void* ptr, const Tiny_NativeProp* prop)
{
    assert(thread && thread->state);
    
    // Make sure thread is alive
    assert(thread->pc >= 0);

	Tiny_Object* obj = NewObject(thread, TINY_VAL_NATIVE);
	
	obj->nat.addr = ptr;
	obj->nat.prop = prop;

	Tiny_Value val;

	val.type = TINY_VAL_NATIVE;
	val.obj = obj;

	return val;
}

Tiny_Value Tiny_NewBool(bool value)
{
	Tiny_Value val;

	val.type = TINY_VAL_BOOL;
	val.boolean = value;

	return val;
}

Tiny_Value Tiny_NewNumber(double value)
{
	Tiny_Value val;

	val.type = TINY_VAL_NUM;
	val.number = value;

	return val;
}

Tiny_Value Tiny_NewConstString(const char* str)
{
	assert(str);

	Tiny_Value val;

	val.type = TINY_VAL_CONST_STRING;
	val.cstr = str;
	
	return val;
}

Tiny_Value Tiny_NewString(Tiny_StateThread* thread, char* string)
{
	assert(thread && thread->state && string);
    
    Tiny_Object* obj = NewObject(thread, TINY_VAL_STRING);
	obj->string = string;

	Tiny_Value val;

	val.type = TINY_VAL_STRING;
	val.obj = obj;

	return val;
}

static void Symbol_destroy(Symbol* sym);

Tiny_State* Tiny_CreateState(void)
{
    Tiny_State* state = emalloc(sizeof(Tiny_State));

    state->programLength = 0;

    state->numGlobalVars = 0;
    
    state->numFunctions = 0;
    state->functionPcs = NULL;

    state->numForeignFunctions = 0;
    state->foreignFunctions = NULL;

    state->currScope = 0;
    state->currFunc = NULL;
    state->globalSymbols = NULL;

    state->fileName = NULL;
    state->lineNumber = 0;

    return state;
}

void Tiny_DeleteState(Tiny_State* state)
{
	// Delete all symbols
    for(int i = 0; i < sb_count(state->globalSymbols); ++i) {
		Symbol_destroy(state->globalSymbols[i]);
	}

    sb_free(state->globalSymbols);

    // Reset function and variable data
	free(state->functionPcs);
	free(state->foreignFunctions);

    free(state);
}

void Tiny_InitThread(Tiny_StateThread* thread, const Tiny_State* state)
{
    thread->state = state;

    thread->gcHead = NULL;
    thread->numObjects = 0;
    // TODO: Use INIT_GC_THRESH definition
    thread->maxNumObjects = 8;

    thread->globalVars = NULL;
    
    thread->pc = -1;
    thread->fp = thread->sp = 0;
    
    thread->retVal = Tiny_Null;

    thread->indirStackSize = 0;
	thread->userdata = NULL;
}

static void AllocGlobals(Tiny_StateThread* thread)
{
    // If the global variables haven't been allocated yet,
    // do that
    if(!thread->globalVars)
    {
        thread->globalVars = emalloc(sizeof(Tiny_Value) * thread->state->numGlobalVars);
        memset(thread->globalVars, 0, sizeof(Tiny_Value) * thread->state->numGlobalVars);
    }
}

void Tiny_StartThread(Tiny_StateThread* thread)
{
	AllocGlobals(thread);

    // TODO: Eventually move to an actual entry point
    thread->pc = 0;
}

static bool ExecuteCycle(Tiny_StateThread* thread);

int Tiny_GetGlobalIndex(const Tiny_State* state, const char* name)
{
	for (int i = 0; i < sb_count(state->globalSymbols); ++i) {
		Symbol* sym = state->globalSymbols[i];

		if (sym->type == SYM_GLOBAL && strcmp(sym->name, name) == 0) {
			return sym->var.index;
		}
	}

	return -1;
}

int Tiny_GetFunctionIndex(const Tiny_State* state, const char* name)
{
	for (int i = 0; i < sb_count(state->globalSymbols); ++i) {
		Symbol* sym = state->globalSymbols[i];

		if (sym->type == SYM_FUNCTION && strcmp(sym->name, name) == 0) {
			return sym->func.index;
		}
	}

	return -1;
}

static void DoPushIndir(Tiny_StateThread* thread, int nargs);
static void DoPush(Tiny_StateThread* thread, Tiny_Value value);

Tiny_Value Tiny_GetGlobal(const Tiny_StateThread* thread, int globalIndex)
{
	assert(globalIndex >= 0 && globalIndex < thread->state->numGlobalVars);
	assert(thread->globalVars);
	
	return thread->globalVars[globalIndex];
}

void Tiny_SetGlobal(Tiny_StateThread* thread, int globalIndex, Tiny_Value value)
{
	assert(globalIndex >= 0 && globalIndex < thread->state->numGlobalVars);
	assert(thread->globalVars);

	thread->globalVars[globalIndex] = value;
}

Tiny_Value Tiny_CallFunction(Tiny_StateThread* thread, int functionIndex, const Tiny_Value* args, int count)
{
	assert(thread->state && functionIndex >= 0);

	int pc, fp, sp, indirStackSize;

	pc = thread->pc;
	fp = thread->fp;
	sp = thread->sp;
	indirStackSize = thread->indirStackSize;

	AllocGlobals(thread);

	for (int i = 0; i < count; ++i) {
		DoPush(thread, args[i]);
	}

	thread->pc = thread->state->functionPcs[functionIndex];
	DoPushIndir(thread, count);

	// Keep executing until the indir stack is restored (i.e. function is done)
	while (thread->indirStackSize > indirStackSize) {
		ExecuteCycle(thread);
	}

	Tiny_Value retVal = thread->retVal;

	thread->pc = pc;
	thread->fp = fp;
	thread->sp = sp;
	thread->indirStackSize = indirStackSize;

	return retVal;
}

bool Tiny_ExecuteCycle(Tiny_StateThread* thread)
{
    return ExecuteCycle(thread);
}

void Tiny_DestroyThread(Tiny_StateThread* thread)
{
    thread->pc = -1;

    // Free all objects in the gc list
    while(thread->gcHead)
    {
        Tiny_Object* next = thread->gcHead->next;
        DeleteObject(thread->gcHead);
        thread->gcHead = next;
    }

    // Free all global variables
    free(thread->globalVars);
}

static void MarkAll(Tiny_StateThread* thread)
{
    assert(thread->state);

    Tiny_ProtectFromGC(thread->retVal);

	for (int i = 0; i < thread->sp; ++i)
        Tiny_ProtectFromGC(thread->stack[i]);

    for (int i = 0; i < thread->state->numGlobalVars; ++i)
        Tiny_ProtectFromGC(thread->globalVars[i]);
}

static void GenerateCode(Tiny_State* state, Word inst)
{	
	assert(state->programLength < MAX_PROG_LEN && "Program Overflow!");
    state->program[state->programLength++] = inst;
}

static void GenerateInt(Tiny_State* state, int value)
{
	Word* wp = (Word*)(&value);
	for(int i = 0; i < 4; ++i)
		GenerateCode(state, *wp++);
}

static void GenerateIntAt(Tiny_State* state, int value, int pc)
{
	Word* wp = (Word*)(&value);
	for(int i = 0; i < 4; ++i)
		state->program[pc + i] = *wp++;
}

static int RegisterNumber(double value)
{
    for(int i = 0; i < NumNumbers; ++i)
    {
        if(Numbers[i] == value)
            return i;
    }

    assert(NumNumbers < MAX_NUMBERS);
    Numbers[NumNumbers++] = value;

    return NumNumbers - 1;
}

static int RegisterString(const char* string)
{
    for(int i = 0; i < NumStrings; ++i)
    {
        if(strcmp(Strings[i], string) == 0)
            return i;
    }

    assert(NumStrings < MAX_STRINGS);
    strcpy(Strings[NumStrings++], string);

    return NumStrings - 1; 
}

static Symbol* Symbol_create(SymbolType type, const char* name, const Tiny_State* state)
{
	Symbol* sym = emalloc(sizeof(Symbol));

	sym->name = estrdup(name);
	sym->type = type;

    sym->fileName = state->fileName;
    sym->lineNumber = state->lineNumber;

	return sym;
}

static void Symbol_destroy(Symbol* sym)
{
	if (sym->type == SYM_FUNCTION)
	{
        for(int i = 0; i < sb_count(sym->func.args); ++i) {
            Symbol* arg = sym->func.args[i];

			assert(arg->type == SYM_LOCAL);

			Symbol_destroy(arg);
		}
        
        sb_free(sym->func.args);
	
	    for(int i = 0; i < sb_count(sym->func.locals); ++i) {
            Symbol* local = sym->func.locals[i];

			assert(local->type == SYM_LOCAL);

			Symbol_destroy(local);
		}

        sb_free(sym->func.locals);
	}

	free(sym->name);
	free(sym);
}

static void OpenScope(Tiny_State* state)
{
	++state->currScope;
}

static void CloseScope(Tiny_State* state)
{
	if (state->currFunc)
	{
        for(int i = 0; i < sb_count(state->currFunc->func.locals); ++i) {
            Symbol* sym = state->currFunc->func.locals[i];

            assert(sym->type == SYM_LOCAL);

            if(sym->var.scope == state->currScope) {
                sym->var.scopeEnded = true;
            }
        }
	}

	--state->currScope;
}

static Symbol* ReferenceVariable(Tiny_State* state, const char* name)
{
	if (state->currFunc)
	{
		// Check local variables
		for(int i = 0; i < sb_count(state->currFunc->func.locals); ++i) {
            Symbol* sym = state->currFunc->func.locals[i];

            assert(sym->type == SYM_LOCAL);

			// Make sure that it's available in the current scope too
			if (!sym->var.scopeEnded && strcmp(sym->name, name) == 0) {
				return sym;
            }
		}

		// Check arguments
		for(int i = 0; i < sb_count(state->currFunc->func.args); ++i) {
            Symbol* sym = state->currFunc->func.args[i];

            assert(sym->type == SYM_LOCAL);

			if (strcmp(sym->name, name) == 0) {
				return sym;
            }
		}
	}

	// Check global variables/constants
	for(int i = 0; i < sb_count(state->globalSymbols); ++i) {
        Symbol* sym = state->globalSymbols[i];

		if (sym->type == SYM_GLOBAL || sym->type == SYM_CONST)
		{
			if (strcmp(sym->name, name) == 0)
				return sym;
		}
	}

	// This variable doesn't exist
	return NULL;
}

static void ReportError(Tiny_State* state, const char* s, ...);

static Symbol* DeclareGlobalVar(Tiny_State* state, const char* name)
{
	Symbol* sym = ReferenceVariable(state, name);

    if(sym && (sym->type == SYM_GLOBAL || sym->type == SYM_CONST)) {
        ReportError(state, "Attempted to declare multiple global entities with the same name '%s'.", name);
    }


	Symbol* newNode = Symbol_create(SYM_GLOBAL, name, state);

	newNode->var.initialized = false;
	newNode->var.index = state->numGlobalVars;
	newNode->var.scope = 0;					// Global variable scope don't matter
	newNode->var.scopeEnded = false;

    sb_push(state->globalSymbols, newNode);

	state->numGlobalVars += 1;

	return newNode;
}

// This expects nargs to be known beforehand because arguments are evaluated/pushed left-to-right
// so the first argument is actually at -nargs position relative to frame pointer
// We could reverse it, but this works out nicely for Foreign calls since we can just supply
// a pointer to the initial arg instead of reversing them.
static Symbol* DeclareArgument(Tiny_State* state, const char* name, int nargs)
{
	assert(state->currFunc);

	for(int i = 0; i < sb_count(state->currFunc->func.args); ++i) {
        Symbol* sym = state->currFunc->func.args[i];

		assert(sym->type == SYM_LOCAL);

		if (strcmp(sym->name, name) == 0) {
            ReportError(state, "Function '%s' takes multiple arguments with name '%s'.\n", state->currFunc->name, name);
		}
	}

	Symbol* newNode = Symbol_create(SYM_LOCAL, name, state);

	newNode->var.initialized = false;
	newNode->var.scopeEnded = false;
	newNode->var.index = -nargs + sb_count(state->currFunc->func.args);
	newNode->var.scope = 0;								// These should be accessible anywhere in the function

    sb_push(state->currFunc->func.args, newNode);
	
	return newNode;
}

static Symbol* DeclareLocal(Tiny_State* state, const char* name)
{
	assert(state->currFunc);

	for(int i = 0; i < sb_count(state->currFunc->func.locals); ++i) {
        Symbol* sym = state->currFunc->func.locals[i];

		assert(sym->type == SYM_LOCAL);

		if (!sym->var.scopeEnded && strcmp(sym->name, name) == 0) {
            ReportError(state, "Function '%s' has multiple locals in the same scope with name '%s'.\n", state->currFunc->name, name);
		}
	}

	Symbol* newNode = Symbol_create(SYM_LOCAL, name, state);

	newNode->var.initialized = false;
	newNode->var.scopeEnded = false;
	newNode->var.index = sb_count(state->currFunc->func.locals);
	newNode->var.scope = state->currScope;

    sb_push(state->currFunc->func.locals, newNode);

	return newNode;
}

static Symbol* DeclareConst(Tiny_State* state, const char* name, bool isString, int index)
{
	Symbol* sym = ReferenceVariable(state, name);

	if (sym && (sym->type == SYM_CONST || sym->type == SYM_LOCAL || sym->type == SYM_GLOBAL)) {
        ReportError(state, "Attempted to define constant with the same name '%s' as another value.\n", name);
	}

	if (state->currFunc)
		fprintf(stderr, "Warning: Constant '%s' declared inside function bodies will still have global scope.\n", name);
	
	Symbol* newNode = Symbol_create(SYM_CONST, name, state);

	newNode->constant.index = index;
    newNode->constant.isString = isString;

    sb_push(state->globalSymbols, newNode);

	return newNode;
}

static Symbol* DeclareFunction(Tiny_State* state, const char* name)
{
	Symbol* newNode = Symbol_create(SYM_FUNCTION, name, state);

	newNode->func.index = state->numFunctions;
	newNode->func.args = NULL;
	newNode->func.locals = NULL;

    sb_push(state->globalSymbols, newNode);

	state->numFunctions += 1;

	return newNode;
}

static Symbol* ReferenceFunction(Tiny_State* state, const char* name)
{
    for(int i = 0; i < sb_count(state->globalSymbols); ++i) {
	    Symbol* node = state->globalSymbols[i];

		if ((node->type == SYM_FUNCTION || node->type == SYM_FOREIGN_FUNCTION) &&
			strcmp(node->name, name) == 0)
			return node;
	}

	return NULL;
}

void Tiny_BindFunction(Tiny_State* state, const char* name, Tiny_ForeignFunction func)
{
    for(int i = 0; i < sb_count(state->globalSymbols); ++i) {
        Symbol* node = state->globalSymbols[i];

		if (node->type == SYM_FOREIGN_FUNCTION && strcmp(node->name, name) == 0)
		{
			fprintf(stderr, "There is already a foreign function bound to name '%s'.", name);
			exit(1);
		}
	}

	Symbol* newNode = Symbol_create(SYM_FOREIGN_FUNCTION, name, state);

	newNode->foreignFunc.index = state->numForeignFunctions;
	newNode->foreignFunc.callee = func;

    sb_push(state->globalSymbols, newNode);

	state->numForeignFunctions += 1;
}

void Tiny_BindConstNumber(Tiny_State* state, const char* name, double number)
{
	DeclareConst(state, name, false, RegisterNumber(number));
}

void Tiny_BindConstString(Tiny_State* state, const char* name, const char* string)
{
	DeclareConst(state, name, true, RegisterString(string));
}

enum
{
	OP_PUSH_NULL,
	OP_PUSH_TRUE,
	OP_PUSH_FALSE,

	OP_PUSH_NUMBER,
    OP_PUSH_STRING,

	OP_POP,
	
	OP_ADD,
	OP_SUB,
	OP_MUL,
	OP_DIV,
	OP_MOD,
	OP_OR,
	OP_AND,
	OP_LT,
	OP_LTE,
	OP_GT,
	OP_GTE,
	OP_EQU,

	OP_LOG_NOT,
	OP_LOG_AND,
	OP_LOG_OR,

	OP_PRINT,
	
	OP_SET,
	OP_GET,
	
	OP_READ,
	
	OP_GOTO,
	OP_GOTOZ,

	OP_CALL,
	OP_RETURN,
	OP_RETURN_VALUE,

	OP_CALLF,

	OP_GETLOCAL,
	OP_SETLOCAL,

	OP_GET_RETVAL,

	OP_HALT
};

static int ReadInteger(Tiny_StateThread* thread)
{
    assert(thread->state);

	int val = 0;
	Word* wp = (Word*)(&val);
	for(int i = 0; i < 4; ++i)
	{
		*wp = thread->state->program[thread->pc++];
		++wp;
	}

	return val;
}

static void DoPush(Tiny_StateThread* thread, Tiny_Value value)
{
	if(thread->sp >= MAX_STACK) 
	{
		fprintf(stderr, "Stack Overflow at PC: %i! (Stack size: %i)", thread->pc, thread->sp);
		exit(1);
	}

	thread->stack[thread->sp++] = value;
}

inline Tiny_Value DoPop(Tiny_StateThread* thread)
{
    assert(thread->state);

	if(thread->sp <= 0) 
	{
		fprintf(stderr, "Stack Underflow at PC: %i (Inst %i)!", thread->pc, thread->state->program[thread->pc]);
		exit(1);
	}
    
    return thread->stack[--thread->sp];
}

static void DoRead(Tiny_StateThread* thread)
{
	char* buffer = emalloc(1);
	size_t bufferLength = 0;
	size_t bufferCapacity = 1;
	
	int c = getc(stdin);
	int i = 0;

	while(c != '\n')
	{
		if(bufferLength + 1 >= bufferCapacity)
		{
			bufferCapacity *= 2;
			buffer = erealloc(buffer, bufferCapacity);
		}
		
		buffer[i++] = c;
		c = getc(stdin);
	}
	
	buffer[i] = '\0';
	
	Tiny_Object* obj = NewObject(thread, TINY_VAL_STRING);
	obj->string = buffer;

	Tiny_Value val;

	val.type = TINY_VAL_STRING;
	val.obj = obj;

	DoPush(thread, val);
}

static void DoPushIndir(Tiny_StateThread* thread, int nargs)
{
	assert(thread->indirStackSize + 3 <= TINY_THREAD_INDIR_SIZE);

	thread->indirStack[thread->indirStackSize++] = nargs;
	thread->indirStack[thread->indirStackSize++] = thread->fp;
	thread->indirStack[thread->indirStackSize++] = thread->pc;

	thread->fp = thread->sp;
}

static void DoPopIndir(Tiny_StateThread* thread)
{
	assert(thread->indirStackSize >= 3);

    thread->sp = thread->fp;

	int prevPc = thread->indirStack[--thread->indirStackSize];
	int prevFp = thread->indirStack[--thread->indirStackSize];
	int nargs = thread->indirStack[--thread->indirStackSize];
	
	thread->sp -= nargs;
	thread->fp = prevFp;
	thread->pc = prevPc;
}

inline static bool ExpectBool(const Tiny_Value value)
{
    assert(value.type == TINY_VAL_BOOL);
    return value.boolean;
}

static bool ExecuteCycle(Tiny_StateThread* thread)
{
    assert(thread && thread->state);

	if (thread->pc < 0) return false;

    const Tiny_State* state = thread->state;

	switch(state->program[thread->pc])
	{
		case OP_PUSH_NULL:
		{
			++thread->pc;
			DoPush(thread, Tiny_Null);
		} break;
		
		case OP_PUSH_TRUE:
		{
			++thread->pc;
			DoPush(thread, Tiny_NewBool(true));
		} break;

		case OP_PUSH_FALSE:
		{
			++thread->pc;
			DoPush(thread, Tiny_NewBool(false));
		} break;

        case OP_PUSH_NUMBER:
		{
			++thread->pc;
            
            int numberIndex = ReadInteger(thread);

            DoPush(thread, Tiny_NewNumber(Numbers[numberIndex]));
		} break;

		case OP_PUSH_STRING:
		{
			++thread->pc;

			int stringIndex = ReadInteger(thread);

			DoPush(thread, Tiny_NewConstString(Strings[stringIndex]));
		} break;
		
		case OP_POP:
		{
			DoPop(thread);
			++thread->pc;
		} break;
		
		#define BIN_OP(OP, operator) case OP_##OP: { Tiny_Value val2 = DoPop(thread); Tiny_Value val1 = DoPop(thread); DoPush(thread, Tiny_NewNumber(val1.number operator val2.number)); ++thread->pc; } break;
		#define BIN_OP_INT(OP, operator) case OP_##OP: { Tiny_Value val2 = DoPop(thread); Tiny_Value val1 = DoPop(thread); DoPush(thread, Tiny_NewNumber((int)val1.number operator (int)val2.number)); ++thread->pc; } break;

		#define REL_OP(OP, operator) case OP_##OP: { Tiny_Value val2 = DoPop(thread); Tiny_Value val1 = DoPop(thread); DoPush(thread, Tiny_NewBool(val1.number operator val2.number)); ++thread->pc; } break;

		case OP_MUL:
		{
			Tiny_Value val2 = DoPop(thread);
			Tiny_Value val1 = DoPop(thread);

			DoPush(thread, Tiny_NewNumber(val1.number * val2.number));

			++thread->pc;
		} break;

		BIN_OP(ADD, +)
		BIN_OP(SUB, -)
		BIN_OP(DIV, /)
		BIN_OP_INT(MOD, %)
		BIN_OP_INT(OR, |)
		BIN_OP_INT(AND, &)
		
		case OP_LT: 
		{ 
			Tiny_Value val2 = DoPop(thread); 
			Tiny_Value val1 = DoPop(thread); 
			DoPush(thread, Tiny_NewBool(val1.number < val2.number)); 
			++thread->pc; 
		} break;
		
		REL_OP(LTE, <=)
		REL_OP(GT, >)
		REL_OP(GTE, >=)

		#undef BIN_OP
		#undef BIN_OP_INT
		#undef REL_OP

		case OP_EQU:
		{
			++thread->pc;
			Tiny_Value b = DoPop(thread);
			Tiny_Value a = DoPop(thread);

			bool bothStrings = ((a.type == TINY_VAL_CONST_STRING && b.type == TINY_VAL_STRING) ||
				(a.type == TINY_VAL_STRING && b.type == TINY_VAL_CONST_STRING));

			if (a.type != b.type && !bothStrings)
				DoPush(thread, Tiny_NewBool(false));
			else
			{
				if (a.type == TINY_VAL_NULL)
					DoPush(thread, Tiny_NewBool(true));
				else if (a.type == TINY_VAL_BOOL)
					DoPush(thread, Tiny_NewBool(a.boolean == b.boolean));
				else if (a.type == TINY_VAL_NUM)
					DoPush(thread, Tiny_NewBool(a.number == b.number));
				else if (a.type == TINY_VAL_STRING) 
					DoPush(thread, Tiny_NewBool(strcmp(a.obj->string, Tiny_ToString(b)) == 0));
				else if (a.type == TINY_VAL_CONST_STRING) 
				{
					if (b.type == TINY_VAL_CONST_STRING && a.cstr == b.cstr) DoPush(thread, Tiny_NewBool(true));
					else DoPush(thread, Tiny_NewBool(strcmp(a.cstr, Tiny_ToString(b)) == 0));
				}
				else if (a.type == TINY_VAL_NATIVE)
					DoPush(thread, Tiny_NewBool(a.obj->nat.addr == b.obj->nat.addr));
                else if (a.type == TINY_VAL_LIGHT_NATIVE)
                    DoPush(thread, Tiny_NewBool(a.addr == b.addr));
			}
		} break;

		case OP_LOG_NOT:
		{
			++thread->pc;
			Tiny_Value a = DoPop(thread);

			DoPush(thread, Tiny_NewBool(!ExpectBool(a)));
		} break;

		case OP_LOG_AND:
		{
			++thread->pc;
			Tiny_Value b = DoPop(thread);
			Tiny_Value a = DoPop(thread);

			DoPush(thread, Tiny_NewBool(ExpectBool(a) && ExpectBool(b)));
		} break;

		case OP_LOG_OR:
		{
			++thread->pc;
			Tiny_Value b = DoPop(thread);
			Tiny_Value a = DoPop(thread);

			DoPush(thread, Tiny_NewBool(ExpectBool(a) || ExpectBool(b)));
		} break;

		case OP_PRINT:
		{
			Tiny_Value val = DoPop(thread);
			if(val.type == TINY_VAL_NUM) printf("%g\n", val.number);
			else if (val.obj->type == TINY_VAL_STRING) printf("%s\n", val.obj->string);
			else if (val.obj->type == TINY_VAL_CONST_STRING) printf("%s\n", val.cstr);
			else if (val.obj->type == TINY_VAL_NATIVE) printf("<native at %p>\n", val.obj->nat.addr);
			else if (val.obj->type == TINY_VAL_LIGHT_NATIVE) printf("<light native at %p>\n", val.obj->nat.addr);
			++thread->pc;
		} break;

		case OP_SET:
		{
			++thread->pc;
			int varIdx = ReadInteger(thread);
			thread->globalVars[varIdx] = DoPop(thread);
		} break;
		
		case OP_GET:
		{
			++thread->pc;
			int varIdx = ReadInteger(thread);
			DoPush(thread, thread->globalVars[varIdx]); 
		} break;
		
		case OP_READ:
		{
			DoRead(thread);
			++thread->pc;
		} break;
		
		case OP_GOTO:
		{
			++thread->pc;
			int newPc = ReadInteger(thread);
			thread->pc = newPc;
		} break;
		
		case OP_GOTOZ:
		{
			++thread->pc;
			int newPc = ReadInteger(thread);
			
			Tiny_Value val = DoPop(thread);

			if(!ExpectBool(val))
				thread->pc = newPc;
		} break;
		
		case OP_CALL:
		{
			++thread->pc;
			int nargs = ReadInteger(thread);
			int pcIdx = ReadInteger(thread);
			
			DoPushIndir(thread, nargs);
			thread->pc = state->functionPcs[pcIdx];
		} break;
		
		case OP_RETURN:
		{
            thread->retVal = Tiny_Null;

			DoPopIndir(thread);
		} break;
		
		case OP_RETURN_VALUE:
		{
			thread->retVal = DoPop(thread);
			DoPopIndir(thread);
		} break;
		
		case OP_CALLF:
		{
			++thread->pc;
			
			int nargs = ReadInteger(thread);
			int fIdx = ReadInteger(thread);

			// the state of the stack prior to the function arguments being pushed
			int prevSize = thread->sp - nargs;

            thread->retVal = state->foreignFunctions[fIdx](thread, &thread->stack[prevSize], nargs);
			
			// Resize the stack so that it has the arguments removed
			thread->sp = prevSize;
		} break;

		case OP_GETLOCAL:
		{
			++thread->pc;
			int localIdx = ReadInteger(thread);
			DoPush(thread, thread->stack[thread->fp + localIdx]);
		} break;
		
		case OP_SETLOCAL:
		{
			++thread->pc;
			int localIdx = ReadInteger(thread);
			Tiny_Value val = DoPop(thread);
			thread->stack[thread->fp + localIdx] = val;
		} break;

		case OP_GET_RETVAL:
		{
			++thread->pc;
			DoPush(thread, thread->retVal);
		} break;

		case OP_HALT:
		{
			thread->pc = -1;
		} break;
	}

	// Only collect garbage in between iterations
	if (thread->numObjects >= thread->maxNumObjects)
		GarbageCollect(thread);

	return true;
}

enum
{
	TOK_BEGIN = -1,
	TOK_END = -2,
	TOK_IDENT = -3,
	TOK_DECLARE = -4,		// :=
	TOK_DECLARECONST = -5,	// ::
	TOK_PLUSEQUAL = -6,		// +=
	TOK_MINUSEQUAL = -7,	// -=
	TOK_MULEQUAL = -8,		// *=
	TOK_DIVEQUAL = -9,		// /=
	TOK_MODEQUAL = -10,		// %=
	TOK_OREQUAL	= -11,		// |=
	TOK_ANDEQUAL = -12,		// &=
	TOK_NUMBER = -13,
	TOK_STRING = -14,
	TOK_PROC = -15,
	TOK_IF = -16,
	TOK_EQUALS = -17,
	TOK_NOTEQUALS = -18,
	TOK_LTE = -19,
	TOK_GTE = -20,
	TOK_RETURN = -21,
	TOK_WHILE = -22,
	TOK_FOR = -23,
	TOK_DO = -24,
	TOK_THEN = -25,
	TOK_ELSE = -26,
	TOK_EOF = -27,
	TOK_NOT = -28,
	TOK_AND = -29,
	TOK_OR = -30,
	TOK_NULL = -31,
	TOK_TRUE = -32,
	TOK_FALSE = -33
};

char TokenBuffer[MAX_TOK_LEN];
double TokenNumber;

static int Peek(FILE* in)
{
	int c = getc(in);
	ungetc(c, in);
	return c;
}

// HACK(Apaar): This is used to reset
// the GetToken's last char after
// we've hit the EOF of a file. Otherwise
// it thinks it's EOFd right away.
static bool ResetGetToken = false;

static int GetToken(Tiny_State* state, FILE* in)
{
	static int last = ' ';

	if (ResetGetToken)
	{
		last = ' ';
		ResetGetToken = false;
	}

	while (isspace(last))
	{
		if (last == '\n')
			++state->lineNumber;

		last = getc(in);
	}

	if(isalpha(last))
	{
		int i = 0;
		while(isalnum(last) || last == '_')
		{
			assert(i < MAX_TOK_LEN - 1 && "Token was too long!");
			TokenBuffer[i++] = last;
			last = getc(in);
		}
		TokenBuffer[i] = '\0';
		
		if (strcmp(TokenBuffer, "func") == 0) return TOK_PROC;
		if (strcmp(TokenBuffer, "if") == 0) return TOK_IF;
		if (strcmp(TokenBuffer, "return") == 0) return TOK_RETURN;
		if (strcmp(TokenBuffer, "while") == 0) return TOK_WHILE;
		if (strcmp(TokenBuffer, "for") == 0) return TOK_FOR;
		if (strcmp(TokenBuffer, "else") == 0) return TOK_ELSE;
		if (strcmp(TokenBuffer, "not") == 0) return TOK_NOT;
		if (strcmp(TokenBuffer, "and") == 0) return TOK_AND;
		if (strcmp(TokenBuffer, "or") == 0) return TOK_OR;
		if (strcmp(TokenBuffer, "null") == 0) return TOK_NULL;
		if (strcmp(TokenBuffer, "true") == 0) return TOK_TRUE;
		if (strcmp(TokenBuffer, "false") == 0) return TOK_FALSE;
		
		return TOK_IDENT;
	}
	
	if(isdigit(last))
	{
		int i = 0;
		while(isdigit(last) || last == '.')
		{
			assert(i < MAX_TOK_LEN - 1 && "Number was too long!");
			TokenBuffer[i++] = last;
			last = getc(in);
		}
		TokenBuffer[i] = '\0';
		
		TokenNumber = strtod(TokenBuffer, NULL);
		return TOK_NUMBER;
	}

	if (last == '\'')
	{
		last = getc(in);	
        
        if(last == '\\') {
            last = getc(in);
            switch(last) {
                case '\'': last = '\''; break;
                case 'n': last = '\n'; break;
                case 'r': last = '\r'; break;
                case 't': last = '\t'; break;
                case 'b': last = '\b'; break;
                case 'a': last = '\a'; break;
                case 'v': last = '\v'; break;
                case 'f': last = '\f'; break;
                case '\\': last = '\\'; break;
                case '"': last = '"'; break;
            }
        }

		TokenNumber = (double)last;	
		last = getc(in);

		if (last != '\'')
		{
			ReportError(state, "Expected ' to follow previous '.");
		}
		last = getc(in);

		return TOK_NUMBER;
	}
	
	if(last == '"')
	{
		last = getc(in);
		int i = 0;
		while(last != '"')
		{
			if (last == '\\')
			{
				last = getc(in);

				switch (last)
				{
					case 'n': last = '\n'; break;
					case 'r': last = '\r'; break;
					case 't': last = '\t'; break;
					case 'b': last = '\b'; break;
					case 'a': last = '\a'; break;
					case 'v': last = '\v'; break;
					case 'f': last = '\f'; break;
					case '\\': last = '\\'; break;
					case '"': last = '"'; break;
					default:
						if (isdigit(last)) // Octal number
						{
							int n1 = last - '0';
							last = getc(in);

							if (!isdigit(last))
							{
								fprintf(stderr, "Expected three digits in octal escape sequence but only got one.\n");
								exit(1);
							}

							int n2 = last - '0';
							last = getc(in);

							if (!isdigit(last))
							{
								fprintf(stderr, "Expected three digits in octal escape sequence but only got two.\n");
								exit(1);
							}

							int n3 = last - '0';
							last = n3 + n2 * 8 + n1 * 8 * 8;
						}
						else
						{
							fprintf(stderr, "Unsupported escape sequence '\\%c'.\n", last);
							exit(1);
						}
						break;
				}
			}

			TokenBuffer[i++] = last;
			last = getc(in);
		}
		TokenBuffer[i] = '\0';
		
		last = getc(in);
		return TOK_STRING;
	}
	
	if(last == EOF)
		return TOK_EOF;
	
	if(last == '=')
	{
		if(Peek(in) == '=')
		{
			last = getc(in);
			last = getc(in);
			return TOK_EQUALS;
		}
	}
	
	if(last == '!')
	{
		if(Peek(in) == '=')
		{
			last = getc(in);
			last = getc(in);
			return TOK_NOTEQUALS;
		}
	}
	
	if(last == '<')
	{
		if(Peek(in) == '=')
		{
			last = getc(in);
			last = getc(in);
			return TOK_LTE;
		}
	}
	
	if(last == '>')
	{
		if(Peek(in) == '=')
		{
			last = getc(in);
			last = getc(in);
			return TOK_GTE;
		}
	}

	if (last == ':')
	{
		if (Peek(in) == '=')
		{
			getc(in);
			last = getc(in);
			return TOK_DECLARE;
		}
		else if (Peek(in) == ':')
		{
			getc(in);
			last = getc(in);
			return TOK_DECLARECONST;
		}
	}

	if (last == '+')
	{
		if (Peek(in) == '=')
		{
			getc(in);
			last = getc(in);
			return TOK_PLUSEQUAL;
		}
	}
	
	if (last == '-')
	{
		if (Peek(in) == '=')
		{
			getc(in);
			last = getc(in);
			return TOK_MINUSEQUAL;
		}
	}

	if (last == '*')
	{
		if (Peek(in) == '=')
		{
			getc(in);
			last = getc(in);
			return TOK_MULEQUAL;
		}
	}
    
	if (last == '/')
	{
		if (Peek(in) == '=')
		{
			getc(in);
			last = getc(in);
			return TOK_DIVEQUAL;
		}
        else if(Peek(in) == '/')
        {
            getc(in);
            last = getc(in);

            while(last != '\n' && last != EOF) last = getc(in);
            return GetToken(state, in);
        }
	}

	if (last == '%')
	{
		if (Peek(in) == '=')
		{
			getc(in);
			last = getc(in);
			return TOK_MODEQUAL;
		}
	}

	if (last == '&')
	{
		if (Peek(in) == '=')
		{
			getc(in);
			last = getc(in);
			return TOK_ANDEQUAL;
		}
	}

	if (last == '|')
	{
		if (Peek(in) == '=')
		{
			getc(in);
			last = getc(in);
			return TOK_OREQUAL;
		}
	}

	int lastChar = last;
	last = getc(in);
	return lastChar;
}

typedef enum
{
	EXP_ID,
	EXP_CALL,
	EXP_NULL,
	EXP_BOOL,
	EXP_NUM,
	EXP_STRING,
	EXP_BINARY,
	EXP_PAREN,
	EXP_BLOCK,
	EXP_PROC,
	EXP_IF,
	EXP_UNARY,
	EXP_RETURN,
	EXP_WHILE,
	EXP_FOR,
} ExprType;

typedef struct sExpr
{
	ExprType type;

    const char* fileName;
    int lineNumber;

	union
	{
		bool boolean;

		int number;
		int string;

		struct
		{
			char* name;
			Symbol* sym;
		} id;

		struct
		{
			char* calleeName;
            struct sExpr** args; // array
		} call;
		
		struct
		{
			struct sExpr* lhs;
			struct sExpr* rhs;
			int op;
		} binary;
		
		struct sExpr* paren;
		
		struct 
		{
			int op;
			struct sExpr* exp;
		} unary;
		
        struct sExpr** block;    // array

		struct
		{
			Symbol* decl;
			struct sExpr* body;
		} proc;

		struct
		{
			struct sExpr* cond;
			struct sExpr* body;
			struct sExpr* alt;
		} ifx;
		
		struct
		{
			struct sExpr* cond;
			struct sExpr* body;
		} whilex;

		struct
		{
			struct sExpr* init;
			struct sExpr* cond;
			struct sExpr* step;
			struct sExpr* body;
		} forx;
		
		struct sExpr* retExpr;
	};
} Expr;

static Expr* Expr_create(ExprType type, const Tiny_State* state)
{
	Expr* exp = emalloc(sizeof(Expr));

    exp->fileName = state->fileName;
	exp->lineNumber = state->lineNumber;
	exp->type = type;
    
	return exp;
}

int CurTok;

static int GetNextToken(Tiny_State* state, FILE* in)
{
	CurTok = GetToken(state, in);
	return CurTok;
}

static Expr* ParseExpr(Tiny_State* state, FILE* in);

static void ReportErrorV(FILE* f, const char* fileName, int line, const char* s, va_list args)
{
	rewind(f);

	int last;
	
    fputc('\n', stderr);

    for(int i = 0; i < line + 4; ++i) {
        last = getc(f);
		
		bool putLine = abs(line - (i + 1)) < 3;
		
		if (putLine) {
			if (i == line - 1) {
				fprintf(stderr, "%d ->\t", i + 1);
			} else {
				fprintf(stderr, "%d\t", i + 1);
			}
		}

        while(last != '\n') {
			// Print all lines within 3 lines
			if (putLine) {
				fputc(last, stderr);
			}

            last = getc(f);
        }

		if (putLine) {
			fputc('\n', stderr);
		}
    }

	fputc('\n', stderr);

	fprintf(stderr, "%s(%i): ", fileName, line);

    vfprintf(stderr, s, args);
    fputc('\n', stderr); 
}

static void ReportError(Tiny_State* state, const char* s, ...)
{
    va_list args;
    va_start(args, s);

    ReportErrorV(state->curFile, state->fileName, state->lineNumber, s, args);

    va_end(args);
	exit(1);
}

static void ReportErrorE(Tiny_State* state, const Expr* exp, const char* s, ...)
{
    va_list args;
    va_start(args, s);

    ReportErrorV(state->curFile, exp->fileName, exp->lineNumber, s, args);

    va_end(args);
	exit(1);
}

static void ReportErrorS(Tiny_State* state, const Symbol* sym, const char* s, ...)
{
    va_list args;
    va_start(args, s);

    ReportErrorV(state->curFile, sym->fileName, sym->lineNumber, s, args);

    va_end(args);
	exit(1);
}

static void ExpectToken(Tiny_State* state, int tok, const char* msg)
{
	if (CurTok != tok)
	{
        ReportError(state, msg);
	}
}

static Expr* ParseIf(Tiny_State* state, FILE* in)
{
	Expr* exp = Expr_create(EXP_IF, state);

	GetNextToken(state, in);

	exp->ifx.cond = ParseExpr(state, in);
	exp->ifx.body = ParseExpr(state, in);

	if (CurTok == TOK_ELSE)
	{
		GetNextToken(state, in);
		exp->ifx.alt = ParseExpr(state, in);
	}
	else
		exp->ifx.alt = NULL;

	return exp;
}

static Expr* ParseFactor(Tiny_State* state, FILE* in)
{
	switch(CurTok)
	{
		case TOK_NULL:
		{
			Expr* exp = Expr_create(EXP_NULL, state);

			GetNextToken(state, in);

			return exp;
		} break;

		case TOK_TRUE:
		case TOK_FALSE:
		{
			Expr* exp = Expr_create(EXP_BOOL, state);

			exp->boolean = CurTok == TOK_TRUE;

			GetNextToken(state, in);

			return exp;
		} break;

		case '{':
		{
			Expr* exp = Expr_create(EXP_BLOCK, state);

            exp->block = NULL;

			GetNextToken(state, in);

			OpenScope(state);

			while (CurTok != '}')
			{
				Expr* e = ParseExpr(state, in);
                sb_push(exp->block, e);
			}

			GetNextToken(state, in);

			CloseScope(state);

			return exp;
		} break;

		case TOK_IDENT:
		{
			char* ident = estrdup(TokenBuffer);
			GetNextToken(state, in);
			if(CurTok != '(')
			{
				Expr* exp;
				
				exp = Expr_create(EXP_ID, state);
				
				exp->id.sym = ReferenceVariable(state, ident);
				exp->id.name = ident;

				return exp;
			}
			
			Expr* exp = Expr_create(EXP_CALL, state);

			exp->call.args = NULL;
			
			GetNextToken(state, in);
			
			while(CurTok != ')')
			{
				sb_push(exp->call.args, ParseExpr(state, in));

				if(CurTok == ',') GetNextToken(state, in);
				else if(CurTok != ')')
				{
                    ReportError(state, "Expected ')' after call.");
				}
			}

			exp->call.calleeName = ident;

			GetNextToken(state, in);
			return exp;
		} break;
		
		case '-': case '+': case TOK_NOT:
		{
			int op = CurTok;
			GetNextToken(state, in);
			Expr* exp = Expr_create(EXP_UNARY, state);
			exp->unary.op = op;
			exp->unary.exp = ParseFactor(state, in);

			return exp;
		} break;
		
		case TOK_NUMBER:
		{
			Expr* exp = Expr_create(EXP_NUM, state);
			exp->number = RegisterNumber(TokenNumber);
			GetNextToken(state, in);
			return exp;
		} break;

		case TOK_STRING:
		{
			Expr* exp = Expr_create(EXP_STRING, state);
			exp->string = RegisterString(TokenBuffer);
			GetNextToken(state, in);
			return exp;
		} break;
		
		case TOK_PROC:
		{
			if(state->currFunc)
			{
				ReportError(state, "Attempted to define function inside of function '%s'.", state->currFunc->name);
			}
			
			Expr* exp = Expr_create(EXP_PROC, state);
			
			GetNextToken(state, in);

			ExpectToken(state, TOK_IDENT, "Function name must be identifier!");
			
			exp->proc.decl = DeclareFunction(state, TokenBuffer);
			state->currFunc = exp->proc.decl;

			GetNextToken(state, in);
			
			ExpectToken(state, '(', "Expected '(' after function name");
			GetNextToken(state, in);

			int nargs = 0;
			char* argNames[MAX_ARGS] = { 0 };
			
			while(CurTok != ')')
			{
				ExpectToken(state, TOK_IDENT, "Expected identifier in function parameter list");
				if (nargs >= MAX_ARGS) {
                    ReportError(state, "Function '%s' takes in too many args.", exp->proc.decl->name);
				}

				argNames[nargs++] = estrdup(TokenBuffer);
				GetNextToken(state, in);
				
				if (CurTok != ')' && CurTok != ',')
				{
					ReportError(state, "Expected ')' or ',' after parameter name in function parameter list.");
				}

				if(CurTok == ',') GetNextToken(state, in);
			}

			for (int i = 0; i < nargs; ++i) {
				DeclareArgument(state, argNames[i], nargs);
				free(argNames[i]);
			}
			
			GetNextToken(state, in);

			OpenScope(state);
			
			exp->proc.body = ParseExpr(state, in);

			CloseScope(state);

			state->currFunc = NULL;

			return exp;
		} break;
		
		case TOK_IF:
		{
			return ParseIf(state, in);
		} break;
		
		case TOK_WHILE:
		{
			GetNextToken(state, in);
			Expr* exp = Expr_create(EXP_WHILE, state);

			exp->whilex.cond = ParseExpr(state, in);

			OpenScope(state);
			
			exp->whilex.body = ParseExpr(state, in);
			
			CloseScope(state);


			return exp;
		} break;

		case TOK_FOR:
		{
			GetNextToken(state, in);
			Expr* exp = Expr_create(EXP_FOR, state);
			
			// Every local declared after this is scoped to the for
			OpenScope(state);

			exp->forx.init = ParseExpr(state, in);

			ExpectToken(state, ';', "Expected ';' after for initializer.");

			GetNextToken(state, in);

			exp->forx.cond = ParseExpr(state, in);

			ExpectToken(state, ';', "Expected ';' after for condition.");

			GetNextToken(state, in);

			exp->forx.step = ParseExpr(state, in);

			exp->forx.body = ParseExpr(state, in);

			CloseScope(state);

			return exp;
		} break;
		
		case TOK_RETURN:
		{
			GetNextToken(state, in);
			Expr* exp = Expr_create(EXP_RETURN, state);
			if(CurTok == ';')
			{
				GetNextToken(state, in);	
				exp->retExpr = NULL;
				return exp;
			}

			exp->retExpr = ParseExpr(state, in);
			return exp;
		} break;

		case '(':
		{
			GetNextToken(state, in);
			Expr* inner = ParseExpr(state, in);
			assert(CurTok == ')' && "Expected matching ')' after previous '('");
			GetNextToken(state, in);
			
			Expr* exp = Expr_create(EXP_PAREN, state);
			exp->paren = inner;
			return exp;
		} break;
		
		default: break;
	}

	ReportError(state, "Unexpected token %i (%c)\n", CurTok, CurTok);
	return NULL;
}

static int GetTokenPrec()
{
	int prec = -1;
	switch(CurTok)
	{
		case '*': case '/': case '%': case '&': case '|': prec = 5; break;
		
		case '+': case '-':				prec = 4; break;
		
		case TOK_LTE: case TOK_GTE:
		case TOK_EQUALS: case TOK_NOTEQUALS:
		case '<': case '>':				prec = 3; break;
		
		case TOK_AND: case TOK_OR:		prec = 2; break;

		case TOK_PLUSEQUAL: case TOK_MINUSEQUAL: case TOK_MULEQUAL: case TOK_DIVEQUAL:
		case TOK_MODEQUAL: case TOK_ANDEQUAL: case TOK_OREQUAL:
		case TOK_DECLARECONST:
		case TOK_DECLARE: case '=':						prec = 1; break;
	}
	
	return prec;
}

static Expr* ParseBinRhs(Tiny_State* state, FILE* in, int exprPrec, Expr* lhs)
{
	while(1)
	{
		int prec = GetTokenPrec();
		
		if(prec < exprPrec)
			return lhs;

		int binOp = CurTok;

		// They're trying to declare a variable (we can only know this when we 
		// encounter this token)
		if (binOp == TOK_DECLARE)
		{
			if (lhs->type != EXP_ID)
			{
				ReportError(state, "Expected identifier to the left-hand side of ':='.\n");
			}

			// If we're inside a function declare a local, otherwise a global
			if (state->currFunc)
				lhs->id.sym = DeclareLocal(state, lhs->id.name);
			else
				lhs->id.sym = DeclareGlobalVar(state, lhs->id.name);
		}

		GetNextToken(state, in);

		Expr* rhs = ParseFactor(state, in);
		int nextPrec = GetTokenPrec();
		
		if(prec < nextPrec)
			rhs = ParseBinRhs(state, in, prec + 1, rhs);

		if (binOp == TOK_DECLARECONST)
		{
			if (lhs->type != EXP_ID)
			{
				ReportError(state, "Expected identifier to the left-hand side of '::'.\n");
			}

			if (rhs->type == EXP_NUM)
				DeclareConst(state, lhs->id.name, false, rhs->number);
			else if (rhs->type == EXP_STRING)
				DeclareConst(state, lhs->id.name, true, rhs->string);
			else
			{
				ReportError(state, "Expected number or string to be bound to constant '%s'.\n", lhs->id.name);
			}
		}

		Expr* newLhs = Expr_create(EXP_BINARY, state);
		
		newLhs->binary.lhs = lhs;
		newLhs->binary.rhs = rhs;
		newLhs->binary.op = binOp;
		
		lhs = newLhs;
	}
}

static Expr* ParseExpr(Tiny_State* state, FILE* in)
{
	Expr* factor = ParseFactor(state, in);
	return ParseBinRhs(state, in, 0, factor);
}

static Expr** ParseProgram(Tiny_State* state, FILE* in)
{
	ResetGetToken = true;
	GetNextToken(state, in);
		
	if(CurTok != TOK_EOF)
	{	
        Expr** arr = NULL;

		while(CurTok != TOK_EOF)
		{
			Expr* stmt = ParseExpr(state, in);
            sb_push(arr, stmt);
		}

		return arr;
	}

	return NULL;
}

static void CompileProgram(Tiny_State* state, Expr** program);

static void CompileGetId(Tiny_State* state, Expr* exp)
{
	assert(exp->type == EXP_ID);

	if (!exp->id.sym)
	{
		ReportErrorE(state, exp, "Referencing undeclared identifier '%s'.\n", exp->id.name);
	}

	assert(exp->id.sym->type == SYM_GLOBAL ||
		exp->id.sym->type == SYM_LOCAL ||
		exp->id.sym->type == SYM_CONST);

	if (exp->id.sym->type != SYM_CONST)
	{
		if (exp->id.sym->type == SYM_GLOBAL)
			GenerateCode(state, OP_GET);
		else if (exp->id.sym->type == SYM_LOCAL)
			GenerateCode(state, OP_GETLOCAL);

		GenerateInt(state, exp->id.sym->var.index);
	}
	else
	{
        if(exp->id.sym->constant.isString)
            GenerateCode(state, OP_PUSH_STRING);
        else
            GenerateCode(state, OP_PUSH_NUMBER);
		GenerateInt(state, exp->id.sym->constant.index);
	}
}

static void CompileExpr(Tiny_State* state, Expr* exp);

static void CompileCall(Tiny_State* state, Expr* exp)
{
	assert(exp->type == EXP_CALL);

	for (int i = 0; i < sb_count(exp->call.args); ++i)
		CompileExpr(state, exp->call.args[i]);

	Symbol* sym = ReferenceFunction(state, exp->call.calleeName);
	if (!sym)
	{
		ReportErrorE(state, exp, "Attempted to call undefined function '%s'.\n", exp->call.calleeName);
	}

	if (sym->type == SYM_FOREIGN_FUNCTION)
	{
		GenerateCode(state, OP_CALLF);
		GenerateInt(state, sb_count(exp->call.args));
		GenerateInt(state, sym->foreignFunc.index);
	}
	else
	{
		GenerateCode(state, OP_CALL);
		GenerateInt(state, sb_count(exp->call.args));
		GenerateInt(state, sym->func.index);
	}
}

static void CompileExpr(Tiny_State* state, Expr* exp)
{
	switch (exp->type)
	{
		case EXP_NULL:
		{
			GenerateCode(state, OP_PUSH_NULL);
		} break;

		case EXP_ID:
		{
			CompileGetId(state, exp);
		} break;

		case EXP_BOOL:
		{
			GenerateCode(state, exp->boolean ? OP_PUSH_TRUE : OP_PUSH_FALSE);
		} break;

		case EXP_NUM:
		{
			GenerateCode(state, OP_PUSH_NUMBER);
			GenerateInt(state, exp->number);
		} break;

		case EXP_STRING:
		{
			GenerateCode(state, OP_PUSH_STRING);
			GenerateInt(state, exp->string);
		} break;

		case EXP_CALL:
		{
			CompileCall(state, exp);
			GenerateCode(state, OP_GET_RETVAL);
		} break;

		case EXP_BINARY:
		{
			switch (exp->binary.op)
			{
				case '+':
				{
					CompileExpr(state, exp->binary.lhs);
					CompileExpr(state, exp->binary.rhs);
					GenerateCode(state, OP_ADD);
				} break;

				case '*':
				{
					CompileExpr(state, exp->binary.lhs);
					CompileExpr(state, exp->binary.rhs);
					GenerateCode(state, OP_MUL);
				} break;

				case '/':
				{
					CompileExpr(state, exp->binary.lhs);
					CompileExpr(state, exp->binary.rhs);
					GenerateCode(state, OP_DIV);
				} break;

				case '%':
				{
					CompileExpr(state, exp->binary.lhs);
					CompileExpr(state, exp->binary.rhs);
					GenerateCode(state, OP_MOD);
				} break;

				case '|':
				{
					CompileExpr(state, exp->binary.lhs);
					CompileExpr(state, exp->binary.rhs);
					GenerateCode(state, OP_OR);
				} break;

				case '&':
				{
					CompileExpr(state, exp->binary.lhs);
					CompileExpr(state, exp->binary.rhs);
					GenerateCode(state, OP_AND);
				} break;

				case '-':
				{
					CompileExpr(state, exp->binary.lhs);
					CompileExpr(state, exp->binary.rhs);
					GenerateCode(state, OP_SUB);
				} break;

				case '<':
				{
					CompileExpr(state, exp->binary.lhs);
					CompileExpr(state, exp->binary.rhs);
					GenerateCode(state, OP_LT);
				} break;

				case '>':
				{
					CompileExpr(state, exp->binary.lhs);
					CompileExpr(state, exp->binary.rhs);
					GenerateCode(state, OP_GT);
				} break;


				case TOK_EQUALS:
				{
					CompileExpr(state, exp->binary.lhs);
					CompileExpr(state, exp->binary.rhs);
					GenerateCode(state, OP_EQU);
				} break;

				case TOK_NOTEQUALS:
				{
					CompileExpr(state, exp->binary.lhs);
					CompileExpr(state, exp->binary.rhs);
					GenerateCode(state, OP_EQU);
					GenerateCode(state, OP_LOG_NOT);
				} break;

				case TOK_LTE:
				{
					CompileExpr(state, exp->binary.lhs);
					CompileExpr(state, exp->binary.rhs);
					GenerateCode(state, OP_LTE);
				} break;

				case TOK_GTE:
				{
					CompileExpr(state, exp->binary.lhs);
					CompileExpr(state, exp->binary.rhs);
					GenerateCode(state, OP_GTE);
				} break;

				case TOK_AND:
				{
					CompileExpr(state, exp->binary.lhs);
					CompileExpr(state, exp->binary.rhs);
					GenerateCode(state, OP_LOG_AND);
				} break;

				case TOK_OR:
				{
					CompileExpr(state, exp->binary.lhs);
					CompileExpr(state, exp->binary.rhs);
					GenerateCode(state, OP_LOG_OR);
				} break;

				default:
					ReportErrorE(state, exp, "Found assignment when expecting expression.\n");
					break;
			}
		} break;

		case EXP_PAREN:
		{
			CompileExpr(state, exp->paren);
		} break;

		case EXP_UNARY:
		{
			CompileExpr(state, exp->unary.exp);
			switch (exp->unary.op)
			{
				case '-':
				{
					GenerateCode(state, OP_PUSH_NUMBER);
					GenerateInt(state, RegisterNumber(-1));
					GenerateCode(state, OP_MUL);
				} break;

				case TOK_NOT:
				{
					GenerateCode(state, OP_LOG_NOT);
				} break;

				default:
					ReportErrorE(state, exp, "Unsupported unary operator %c (%d)\n", exp->unary.op, exp->unary.op);
					break;
			}
		} break;

        default:
            ReportErrorE(state, exp, "Got statement when expecting expression.\n");
            break;
	}
}

static void CompileStatement(Tiny_State* state, Expr* exp)
{
	switch(exp->type)
	{
		case EXP_CALL:
		{
			CompileCall(state, exp);
		} break;

		case EXP_BLOCK:
		{
            for(int i = 0; i < sb_count(exp->block); ++i) {
				CompileStatement(state, exp->block[i]);
			}
		} break;

		case EXP_BINARY:
		{	
			switch(exp->binary.op)
			{
				case TOK_DECLARECONST:
					// Constants generate no code
					break;
				
				case '=': case TOK_DECLARE: // These two are handled identically in terms of code generated

				case TOK_PLUSEQUAL: case TOK_MINUSEQUAL: case TOK_MULEQUAL: case TOK_DIVEQUAL:
				case TOK_MODEQUAL: case TOK_ANDEQUAL: case TOK_OREQUAL:
				{
					if (exp->binary.lhs->type == EXP_ID)
					{
						switch (exp->binary.op)
						{
							case TOK_PLUSEQUAL:
							{
								CompileGetId(state, exp->binary.lhs);
								CompileExpr(state, exp->binary.rhs);
								GenerateCode(state, OP_ADD);
							} break;

							case TOK_MINUSEQUAL:
							{
								CompileGetId(state, exp->binary.lhs);
								CompileExpr(state, exp->binary.rhs);
								GenerateCode(state, OP_SUB);
							} break;

							case TOK_MULEQUAL:
							{
								CompileGetId(state, exp->binary.lhs);
								CompileExpr(state, exp->binary.rhs);
								GenerateCode(state, OP_MUL);
							} break;

							case TOK_DIVEQUAL:
							{
								CompileGetId(state, exp->binary.lhs);
								CompileExpr(state, exp->binary.rhs);
								GenerateCode(state, OP_DIV);
							} break;

							case TOK_MODEQUAL:
							{
								CompileGetId(state, exp->binary.lhs);
								CompileExpr(state, exp->binary.rhs);
								GenerateCode(state, OP_MOD);
							} break;

							case TOK_ANDEQUAL:
							{
								CompileGetId(state, exp->binary.lhs);
								CompileExpr(state, exp->binary.rhs);
								GenerateCode(state, OP_AND);
							} break;

							case TOK_OREQUAL:
							{
								CompileGetId(state, exp->binary.lhs);
								CompileExpr(state, exp->binary.rhs);
								GenerateCode(state, OP_OR);
							} break;

							default:
								CompileExpr(state, exp->binary.rhs);
								break;
						}

						if (!exp->binary.lhs->id.sym)
						{
							// The variable being referenced doesn't exist
							ReportErrorE(state, exp, "Assigning to undeclared identifier '%s'.\n", exp->binary.lhs->id.name);
						}

						if (exp->binary.lhs->id.sym->type == SYM_GLOBAL)
							GenerateCode(state, OP_SET);
						else if (exp->binary.lhs->id.sym->type == SYM_LOCAL)
							GenerateCode(state, OP_SETLOCAL);
						else		// Probably a constant, can't change it
						{
							ReportErrorE(state, exp, "Cannot assign to id '%s'.\n", exp->binary.lhs->id.name);
						}

						GenerateInt(state, exp->binary.lhs->id.sym->var.index);
						exp->binary.lhs->id.sym->var.initialized = true;
					}
					else
					{
						ReportErrorE(state, exp, "LHS of assignment operation must be a variable\n");
					}
				} break;

				default:
					ReportErrorE(state, exp, "Invalid operation when expecting statement.\n");
					break;
			}
		} break;
		
		case EXP_PROC:
		{
			GenerateCode(state, OP_GOTO);
			int skipGotoPc = state->programLength;
			GenerateInt(state, 0);
			
			state->functionPcs[exp->proc.decl->func.index] = state->programLength;
			
			for(int i = 0; i < sb_count(exp->proc.decl->func.locals); ++i)
			{
				GenerateCode(state, OP_PUSH_NUMBER);
				GenerateInt(state, RegisterNumber(0));
			}
			
			if (exp->proc.body)
				CompileStatement(state, exp->proc.body);

			GenerateCode(state, OP_RETURN);
			GenerateIntAt(state, state->programLength, skipGotoPc);
		} break;
		
		case EXP_IF:
		{
			CompileExpr(state, exp->ifx.cond);
			GenerateCode(state, OP_GOTOZ);
			
			int skipGotoPc = state->programLength;
			GenerateInt(state, 0);
			
			if(exp->ifx.body)
				CompileStatement(state, exp->ifx.body);
			
			GenerateCode(state, OP_GOTO);
			int exitGotoPc = state->programLength;
			GenerateInt(state, 0);

			GenerateIntAt(state, state->programLength, skipGotoPc);

			if (exp->ifx.alt)
				CompileStatement(state, exp->ifx.alt);

			GenerateIntAt(state, state->programLength, exitGotoPc);
		} break;
		
		case EXP_WHILE:
		{
			int condPc = state->programLength;
			
			CompileExpr(state, exp->whilex.cond);
			
			GenerateCode(state, OP_GOTOZ);
			int skipGotoPc = state->programLength;
			GenerateInt(state, 0);
			
			if(exp->whilex.body)
				CompileStatement(state, exp->whilex.body);
			
			GenerateCode(state, OP_GOTO);
			GenerateInt(state, condPc);

			GenerateIntAt(state, state->programLength, skipGotoPc);
		} break;
		
		case EXP_FOR:
		{
			CompileStatement(state, exp->forx.init);

			int condPc = state->programLength;
			CompileExpr(state, exp->forx.cond);

			GenerateCode(state, OP_GOTOZ);
			int skipGotoPc = state->programLength;
			GenerateInt(state, 0);

			if (exp->forx.body)
				CompileStatement(state, exp->forx.body);

			CompileStatement(state, exp->forx.step);
			
			GenerateCode(state, OP_GOTO);
			GenerateInt(state, condPc);

			GenerateIntAt(state, state->programLength, skipGotoPc);
		} break;

		case EXP_RETURN:
		{
			if(exp->retExpr)
			{
				CompileExpr(state, exp->retExpr);
				GenerateCode(state, OP_RETURN_VALUE);
			}
			else
				GenerateCode(state, OP_RETURN);
		} break;

        default:
            ReportErrorE(state, exp, "Got expression when expecting statement.\n");
            break;
	}
}

static void CompileProgram(Tiny_State* state, Expr** program)
{
	Expr** arr = program;
    for(int i = 0; i < sb_count(arr); ++i)  {
		CompileStatement(state, arr[i]);
	}
}

static void DeleteProgram(Expr** program);

static void Expr_destroy(Expr* exp)
{
	switch(exp->type)
	{
		case EXP_ID: 
		{
			free(exp->id.name);
		} break;

		case EXP_NULL: case EXP_BOOL: case EXP_NUM: case EXP_STRING: break;
		
		case EXP_CALL: 
		{
			free(exp->call.calleeName);
			for(int i = 0; i < sb_count(exp->call.args); ++i)
				Expr_destroy(exp->call.args[i]);

            sb_free(exp->call.args);
		} break;

		case EXP_BLOCK:
		{
            for(int i = 0; i < sb_count(exp->block); ++i) {
				Expr_destroy(exp->block[i]);
			}

            sb_free(exp->block);
		} break;
		
		case EXP_BINARY: Expr_destroy(exp->binary.lhs); Expr_destroy(exp->binary.rhs); break;
		case EXP_PAREN: Expr_destroy(exp->paren); break;
		
		case EXP_PROC: 
		{
			if (exp->proc.body) 
				Expr_destroy(exp->proc.body);
		} break;

		case EXP_IF: Expr_destroy(exp->ifx.cond); if (exp->ifx.body) Expr_destroy(exp->ifx.body); if (exp->ifx.alt) Expr_destroy(exp->ifx.alt); break;
		case EXP_WHILE: Expr_destroy(exp->whilex.cond); if(exp->whilex.body) Expr_destroy(exp->whilex.body); break;
		case EXP_RETURN: if(exp->retExpr) Expr_destroy(exp->retExpr); break;
		case EXP_UNARY: Expr_destroy(exp->unary.exp); break;

		case EXP_FOR:
		{
			Expr_destroy(exp->forx.init);
			Expr_destroy(exp->forx.cond);
			Expr_destroy(exp->forx.step);

			Expr_destroy(exp->forx.body);
		} break;

		default: assert(false); break;
	}
	free(exp);
}

void DeleteProgram(Expr** program)
{
	Expr** arr = program;
    for(int i = 0; i < sb_count(program); ++i) {
		Expr_destroy(arr[i]);
	}

    sb_free(program);
}

static void DebugMachineProgram(Tiny_State* state)
{
	for(int i = 0; i < state->programLength; ++i)
	{
		switch(state->program[i])
		{
            case OP_PUSH_NUMBER:	printf("push_number\n"); break;
            case OP_PUSH_STRING:    printf("push_string\n"); break;
			case OP_POP:			printf("pop\n"); break;
			case OP_ADD:			printf("add\n"); break;
			case OP_SUB:			printf("sub\n"); break;
			case OP_MUL:			printf("mul\n"); break;
			case OP_DIV:			printf("div\n"); break;
			case OP_EQU:			printf("equ\n"); break;
			case OP_LOG_NOT:		printf("log_not\n"); break;
			case OP_LT:				printf("lt\n"); break;
			case OP_LTE:			printf("lte\n"); break;
			case OP_GT:				printf("gt\n"); break;
			case OP_GTE:			printf("gte\n"); break;
			case OP_PRINT:			printf("print\n"); break;
			case OP_SET:			printf("set\n"); i += 4; break;
			case OP_GET:			printf("get\n"); i += 4; break;
			case OP_READ:			printf("read\n"); break;
			case OP_GOTO:			printf("goto\n"); i += 4; break;
			case OP_GOTOZ:			printf("gotoz\n"); i += 4; break;
			case OP_CALL:			printf("call\n"); i += 8; break;
			case OP_RETURN:			printf("return\n"); break;
			case OP_RETURN_VALUE:	printf("return_value\n"); break;
			case OP_GETLOCAL:		printf("getlocal\n"); i += 4; break;
			case OP_SETLOCAL:		printf("setlocal\n"); i += 4; break;
			case OP_HALT:			printf("halt\n");
		}
	}
}

static void CheckInitialized(Tiny_State* state)
{
	const char* fmt = "Attempted to use uninitialized variable '%s'.\n";

    for(int i = 0; i < sb_count(state->globalSymbols); ++i) {
        Symbol* node = state->globalSymbols[i];

		assert(node->type != SYM_LOCAL);

		if (node->type == SYM_GLOBAL)
		{
			if (!node->var.initialized)
			{
				ReportErrorS(state, node, fmt, node->name);
			}
		}
		else if (node->type == SYM_FUNCTION)
		{
			// Only check locals, arguments are initialized implicitly
			for(int i = 0; i < sb_count(node->func.locals); ++i) {
                Symbol* local = node->func.locals[i];

				assert(local->type == SYM_LOCAL);

				if (!local->var.initialized)
				{
					ReportErrorS(state, local, fmt, local->name);
				}
			}
		}
	}
}

// Goes through the registered symbols (GlobalSymbols) and assigns all foreign
// functions to their respective index in ForeignFunctions
static void BuildForeignFunctions(Tiny_State* state)
{
    for(int i = 0; i < sb_count(state->globalSymbols); ++i) {
        Symbol* node = state->globalSymbols[i];

		if (node->type == SYM_FOREIGN_FUNCTION)
		    state->foreignFunctions[node->foreignFunc.index] = node->foreignFunc.callee;
	}
}

static void CompileState(Tiny_State* state, Expr** prog)
{
    // If this state was already compiled and it ends with an OP_HALT, We'll just overwrite it
    if(state->programLength > 0) {
        if(state->program[state->programLength - 1] == OP_HALT) {
            state->programLength -= 1;
        }
    }
    
	// Allocate room for vm execution info

	// We realloc because this state might be compiled multiple times (if, e.g., Tiny_CompileString is called twice with same state)
	if (state->numFunctions > 0) {
		state->functionPcs = realloc(state->functionPcs, state->numFunctions * sizeof(int));
	}

	if (state->numForeignFunctions > 0) {
		state->foreignFunctions = realloc(state->foreignFunctions, state->numForeignFunctions * sizeof(Tiny_ForeignFunction));
	}
	
	assert(state->numForeignFunctions == 0 || state->foreignFunctions);
	assert(state->numFunctions == 0 || state->functionPcs);

	BuildForeignFunctions(state);

	CompileProgram(state, prog);
	GenerateCode(state, OP_HALT);

	CheckInitialized(state);		// Done after compilation because it might have registered undefined functions during the compilation stage
	
}

void Tiny_CompileString(Tiny_State* state, const char* name, const char* string)
{
    // TODO: Instead of turning this into a string, create
    // a file-like interface for strings or better yet
    // just parse and compile strings
    FILE* file = tmpfile();
    
    fwrite(string, 1, strlen(string), file);
    rewind(file);

    state->lineNumber = 1;
    state->fileName = name;

	state->curFile = file;

    CurTok = 0;
    Expr** prog = ParseProgram(state, file); 
        
    CompileState(state, prog);

    fclose(file);
	state->curFile = NULL;

	DeleteProgram(prog);
}

void Tiny_CompileFile(Tiny_State* state, const char* filename)
{
    FILE* file = fopen(filename, "r");

    if(!file)
    {
        fprintf(stderr, "Error: Unable to open file '%s' for reading\n", filename);
        exit(1);
    }

	state->lineNumber = 1;
	state->fileName = filename;

	state->curFile = file;

	CurTok = 0;
	Expr** prog = ParseProgram(state, file);
    
    CompileState(state, prog);

	DeleteProgram(prog);

	state->curFile = NULL;
}
