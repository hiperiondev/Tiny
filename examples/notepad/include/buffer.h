#pragma once

#define MAX_NUM_LINES       4096
#define MAX_LINE_LENGTH     512
#define MAX_TRACKED_DEFNS	128
#define MAX_DEFN_LENGTH     256

typedef enum
{
    FILE_C,
    FILE_TINY,
    FILE_UNKNOWN
} Filetype;

typedef struct
{ 
    Filetype filetype;

    int numLines;
    char lines[MAX_NUM_LINES][MAX_LINE_LENGTH];

    int numDefns;
    char defns[MAX_TRACKED_DEFNS][MAX_DEFN_LENGTH];
} Buffer;

void InitDefaultBuffer(Buffer* buf);

void OpenFile(Buffer* buf, const char* filename);

const char* GetLine(Buffer* buf, int y);
void SetLine(Buffer* buf, int y, const char* text);

void InsertEmptyLine(Buffer* buf, int y);
void RemoveLine(Buffer* buf, int y);

void InsertChar(Buffer* buf, int x, int y, char ch);
void InsertString(Buffer* buf, int x, int y, const char* str);

void RemoveChar(Buffer* buf, int x, int y);

// Puts a null terminator at pos
void TerminateLine(Buffer* buf, int x, int y);