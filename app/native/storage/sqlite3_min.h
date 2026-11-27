// DASK Browser - minimal SQLite declarations for winsqlite3.dll.
//
// Windows 10+ ships winsqlite3.dll (SQLite maintained by Microsoft); the SDK
// provides the import library (winsqlite3.lib) but no public sqlite3.h. This
// header declares the exact subset we use with signatures identical to the
// official public-domain sqlite3.h (stable C ABI for decades).
#pragma once

#include <cstdint>

extern "C" {

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef int64_t sqlite3_int64;

// Result codes.
#define SQLITE_OK 0
#define SQLITE_CORRUPT 11
#define SQLITE_CONSTRAINT 19
#define SQLITE_NOTADB 26
#define SQLITE_ROW 100
#define SQLITE_DONE 101
// Open flags.
#define SQLITE_OPEN_READWRITE 0x00000002
#define SQLITE_OPEN_CREATE 0x00000004
#define SQLITE_OPEN_FULLMUTEX 0x00010000
// Destructor control for sqlite3_bind_text (copy the string).
typedef void (*sqlite3_destructor_type)(void*);
#define SQLITE_TRANSIENT ((sqlite3_destructor_type)-1)

int sqlite3_open_v2(const char* filename, sqlite3** ppDb, int flags,
                    const char* z_vfs);
int sqlite3_close_v2(sqlite3*);
int sqlite3_busy_timeout(sqlite3*, int ms);
int sqlite3_exec(sqlite3*, const char* sql,
                 int (*callback)(void*, int, char**, char**), void*,
                 char** errmsg);
int sqlite3_prepare_v2(sqlite3* db, const char* z_sql, int n_byte,
                       sqlite3_stmt** pp_stmt, const char** pz_tail);
int sqlite3_step(sqlite3_stmt*);
int sqlite3_reset(sqlite3_stmt*);
int sqlite3_clear_bindings(sqlite3_stmt*);
int sqlite3_finalize(sqlite3_stmt*);

int sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int sqlite3_bind_text(sqlite3_stmt*, int, const char*, int n_byte,
                      sqlite3_destructor_type);
int sqlite3_bind_null(sqlite3_stmt*, int);

sqlite3_int64 sqlite3_column_int64(sqlite3_stmt*, int i_col);
const unsigned char* sqlite3_column_text(sqlite3_stmt*, int i_col);
int sqlite3_column_count(sqlite3_stmt*);

sqlite3_int64 sqlite3_last_insert_rowid(sqlite3*);
int sqlite3_changes(sqlite3*);
const char* sqlite3_errmsg(sqlite3*);
int sqlite3_errcode(sqlite3*);

}  // extern "C"
