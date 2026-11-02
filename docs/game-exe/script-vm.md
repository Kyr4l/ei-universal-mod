# The WorldScript virtual machine and compiler in game.exe (GSWorld, SSC*)

Written 2026-11-01 from the typed Ghidra decompiles of the Russian `EIStarter OBT-1/Engine/game.exe` (modules 0x4BB000-0x4D7000 compiler
and VM core, 0x499000-0x4B7000 evaluators and containers, plus the owners in `CWorldServer` 0x58A000-0x59F000, cited from
[world-server.md](world-server.md)). The *API side* (what each of the 225 script functions does) is [gs-functions.md](gs-functions.md) and is not
repeated: this page is the language, the compiler, the compiled representation, the scheduler and the save format, written for
um-multitool's script editor / validator (`resources/universal-mod/um-dll/mob_script_check.hpp`, `tools/um-multitool/mapedit/script_ast.hpp`) and
a future um-engine script extension. The vendor reference of the language is [../scripting.md](../scripting.md).

Tags: **[V]** read directly in the decompile, **[I]** inferred from several functions, **[?]** guess. Addresses are VAs; offsets are bytes.
"SSC" is the engine's own prefix (FatalError texts say `CScript::LoadFunction`, `SSCCmd::SetParameters`); "GS" = game script.

## How to re-implement it (short version)

1. **One object does everything.** `GSWorld` (0x1F8 bytes, two instances inside `CWorldServer`: `+0x12874` = the zone script VM, `+0x12A6C` =
   the console / one-shot VM) is at once the compiler state, the symbol tables, the program store, the scheduler and the effect bookkeeping.
   `Compiler_CompileText` (0x4D23A0) compiles a text *into* the VM it is called on, appending to whatever is already there; `GSWorld::Tick`
   (0x4BF000) runs it; `GSWorld::Write` / `Read` (0x4C0F90 / 0x4C0D00) save the compiled programs, variables and run states (no text).
2. **Lexer** = whitespace split + the separators `( ) , : =` + `"..."` + the comments `//` and `/* */`, then every raw token is classified by the
   *longest case-insensitive prefix* among the 225 names of the function table `g_0x78BCA0` (0x4CF600): keywords, commands and punctuation all
   live in that table. A number literal is `[-]digits[.digits]` (atof). Everything else is an identifier (id 0). The prefix rule splits identifiers
   that begin with a table name (`Sleepy` -> `Sleep` `y`) [V].
3. **Grammar** (0x4D23A0 top level, 0x4D1330 statements, 0x4CFBC0 one call): `GlobalVars ( name : type, ... )`, `DeclareScript Name ( ... )`,
   `Script Name [ ( params ) ] ( if ( conds ) then ( cmds ) ... )`, `WorldScript ( cmds )`. A statement is a *command* call, a script call,
   `For / ForIf ( ... ) ( cmds )`, or `global = expr`. An argument is a literal, a declared variable, or a nested *function* call of the right
   return type. No `else`, no `if` inside command lists, no operators, no implicit declarations, no strings from functions.
4. **Compiled form = a tree of `SSCCmd` records (0x94)** in `SSCProgram` arrays: function id + four typed argument slot arrays (float / string /
   object / group) + two binding lists (local var -> slot, global var -> slot) + nested-call bindings (child `SSCCmd` -> slot) + a body program
   pointer for `For`. Literals are baked into the slots; variables are *re-bound before every execution* (0x4B3F60), nested calls re-evaluated
   depth first (0x4B4030). Object slots hold *pointers to the variable's reference cell*, which is how `For` and `=` write variables.
5. **Scheduler** (0x4BF000): per tick, (a) every *running* `SSCScript` (WorldScript bodies and then-bodies suspended in a wait) is stepped until it
   waits or ends, (b) every *active* script instance (created by a call `Name(args)`) has its `if/then` pairs tested in order, a true pair's body
   is run at once (and parked in the running list if it waits), until `KillScript()` marks the instance, (c) dead object references are purged,
   (d) effect timers advance. Waits count the script's own run counter (= VM ticks). There is no call stack and no return value: a script call is
   "spawn an instance".
6. **Values.** float32 everywhere (truth = non-zero); string = CString; object = a refcounted `CObject*` cell (null = `none`; deleted objects become
   null at the next purge); group = a pointer-sorted set of such cells (`ObjRefCArray`, 0x14). Temporaries from `GetObject` / `GroupAdd` ... live in
   two pools of the VM, emptied after every command unless a `For` is active.
7. **Persistence**: everything in the zone VM is saved in chunk 12 of the zone save (programs as serialized `SSCCmd` trees, variables with names,
   object references as object ids, active instances and running scripts with pc / counter / sleep state). Nothing of the VM is replicated to
   clients; scripts run on the server only.

## 1. Conventions

| Thing | Value |
|---|---|
| the VM / compiler object | `GSWorld`, 0x1F8 bytes, vtable 0x73D394 (= the vtable of its first member, a `CArray<GSLightRecord>`) [V]. `g_0x7C27D4` = `CWorldServer +0x12874` (zone script, saved), `g_0x7C29CC` = `+0x12A6C` (console `@` / `#` / `$` lines, `SpawnPartyHeroes`, `OnNamedEvent`, `World_ScriptSpawnFlash`, `CTrapObject::Update`) |
| function table | `g_0x78BCA0`: 225 x 0x10 `{char* name, int id, char* signature, char* params}`, null-name terminated; dump in `_cpr/claude-re/full/gsfuncs.tsv` |
| token id | the table id; 0x7A number literal, 0x7B string literal, 0 identifier; punctuation `(` 0x6D `)` 0x6E `,` 0x71 `=` 0x72 `:` 0x73; keywords `if` 0x6F `then` 0x70 `WorldScript` 0x74 `Script` 0x75 `DeclareScript` 0x76 `GlobalVars` 0x77 |
| type codes | in `SSCVars` / bindings: 1 float, 2 string, 3 object, 4 group [V]; in the assignment command 0x78: 0 float, 1 string, 2 object, 3 group [V] |
| signature letters | lower case = parameter (`f s o g`; `O` = an output object that must be a GlobalVars object: `For`, `ForIf`, `Every`, `Any`, `Sum`); a leading capital `F S O G` = return type of a value function [V] |
| module id | `GSWorld +0x1AC`, 0 after `Reset`, ++ after each successful compile; names declared by module N > 0 are stored as `!N!name` [V] |
| error state | `+0x1EC` CString text, `+0x1F0` code, `+0x1F4` line [V] (`GSWorld::GetLastError` 0x456360 copies them in that order) |
| tick | one `GSWorld::Tick` = one `CWorldServer::Update` (the zone VM only while `CWorldServer +0x160` is set, see world-server.md); `Sleep(n)` = n ticks of the script's own runs |
| one-ness | ids 8 (script call) and 0x78 (global assignment) are internal: not in the table, produced by the statement parser [V] |

## 2. The lexer (`Compiler_Tokenize` 0x4CF7A0, `GSLexer_ClassifyToken` 0x4CF600)

Before tokenizing, `Compiler_CompileText` walks the text **line by line** (split at `\n`): a line that *starts* with `#define` (case-sensitive
`strncmp`) is removed and `name` = the next word, `value` = the rest of the line after the white space (to the end of the line) are stored in a
CList sorted longest-name-first (`DefineList_InsertByLength` 0x4D34F0); every other line gets `Compiler_ApplyDefines` (0x4D33E0): a plain
`CString::Find` / replace of every define collected *so far* (defines are not token-aware: they also hit inside longer identifiers and inside string
literals; a define only affects the lines below it). The rewritten text (define lines blank) is what the tokenizer sees, so line numbers stay those
of the `.eis` text [V].

| Element | Rule [V] | Quirks |
|---|---|---|
| white space | CRT `isspace` (the decompile's `_isdigit_like` is the ctype `_SPACE` bit); `\n` counts the line | |
| raw token | from a non-space char up to white space, `\n`, or one of `, ( ) : =` **when it is not the token's first char** | `(abc` is one raw token (fixed by the prefix cut below); inside `"..."` the separators and spaces are kept, a tab ends it |
| string literal | the raw token starts with `"`; `ClassifyToken` scans to the next `"` -> id 0x7B; the quotes are stripped when stored (`Mid(1, len-2)`) | no closing quote: the token is classified as an identifier; chars after the closing quote are cut off as the next token |
| number literal | first char is a digit, `.` or `-` -> id 0x7A; the literal body = digits with at most one `.` (`ClassifyToken`); value = `atof` (0x6E911A, *not* `atoi`) when stored | no exponent (`1e3` -> `1` + `e3`); `-x` -> `-` (value 0) + `x` |
| comment `/* ... */` | only when the raw token *is* `/*`; the end is searched char by char to `*/`, skipping while a `"` toggles string mode | error 1 `End of file before '*/'`; a lone `"` inside a block comment hides `*/` |
| comment `// ...` | only when the raw token *starts with* `//` (case-insensitive compare of 2 chars); skips to `\n` | `x//c` is one identifier `x//c` |
| classification | the **longest** table name that is a case-insensitive *prefix* of the raw token (`strnicmp(text, name, strlen(name))`) gives the id, and the raw token is **cut after that name**; the rest is re-read as the next token | `Sleepy` -> `Sleep` `y`; `ForestUnit` -> `For` `estUnit`; `Runner` -> `Run` `ner`; `IdleTime` -> `Idle` `Time`: **an identifier may not begin with any table name** (checked case-insensitively); `Su0` is safe (`Sub` does not prefix it) |
| identifier | anything else: id 0; may contain `#`, `.`, `-`, `!`, Cyrillic bytes (the table compare is byte-wise) | `i`, `Heroes`, `#OnBriefingComplete`, `q.z3xq3` are plain identifiers |

The token list is an MFC CList (vt 0x73D424) of `GSTokenNode` `{next, prev, CString text, int line}`; the parsers walk it with a cursor
(`GSTokenCList_GetNext` 0x4D6580) and `Compiler_ExpectToken` (0x4CF4D0).

## 3. The grammar as parsed

```
unit        := { "#define" NAME VALUE-to-EOL }* { top }*                       Compiler_CompileText 0x4D23A0
top         := "GlobalVars" "(" vardecls ")"                                    -> GSWorld +0xA0 (names qualified "!N!")
             | "DeclareScript" NAME "(" vardecls ")"                            -> new SSCScriptDecl in +0x8C (error 0xF if the name exists)
             | "Script" NAME "(" pairs ")"                                      when NAME is declared (error 0x10 if it already has pairs)
             | "Script" NAME "(" vardecls ")" "(" pairs ")"                     when NAME is not declared (declares + defines)
             | "WorldScript" "(" commands ")"                                   one per unit (error 0xE); body -> AddProgramNeg, queued by StartScript
vardecls    := [ NAME ":" ("float"|"string"|"object"|"group") { "," NAME ":" type } ]       Compiler_ParseVarDecls 0x4D1FE0 (type words case-sensitive)
pairs       := { "if" "(" conditions ")" "then" "(" commands ")" }+             Compiler_ParseIfThenPairs 0x4D1030 (error 0xB when none)
conditions  := [ fcall { "," fcall } ]                                           Compiler_ParseConditions 0x4D0BF0: each must be an F function (error 9); empty = true
commands    := { statement }                                                     Compiler_ParseCommands 0x4D1330: no separators needed, ")" ends
statement   := COMMAND "(" args ")"                                              table record with params != 0
             | ("For"|"ForIf") "(" args ")" "(" commands ")"                    body -> AddProgram, SSCCmd +0x90
             | SCRIPTNAME "(" args ")"                                           declared script (plain or "!N!" name): id 8
             | GLOBALNAME "=" expr                                               GlobalVars only: id 0x78 (error 3 "Unexpected token '%s'" otherwise)
args        := [ arg { "," arg } ]                                               Compiler_ParseCommand 0x4CFBC0: one arg per signature letter
arg         := NUMBER (f) | STRING (s) | LOCALVAR | GLOBALVAR | FUNCTION "(" args ")" (return letter must match; "O" = GlobalVars object only)
expr        := arg                                                               (ParseCommand in single-expression mode; the type is the variable's)
```

What the parser does **not** accept, in case the validator guesses otherwise [V]: `else`; `if` inside `then` or `WorldScript` bodies; a value
function as a statement (`GetObject(1)` alone is error 3); a command as an argument (commands have no signature -> error 7); an undeclared name as
an argument of any type, including object and group positions (error 7 `Parameter of type %s expected`: there is no implicit "named map
object" lookup and groups are not created by use); assignment to a script parameter or local (error 3); arithmetic operators; `,` after the last
argument before `)` (the `)` is read as an argument name -> error 7). Accepted leniencies: `f()` for a command with an empty signature; the
`#` and `.` characters in names; keywords in any case (`any(...)`, `worldscript`); a script defined before its declaration-free use is not
possible: a call to a name that is not yet declared is error 3, which is why the shipped maps declare everything first.

**Module qualification.** `Compiler_QualifyName` (0x4D37F0) turns `name` into `!N!name` when `GSWorld +0x1AC` = N is non-zero. GlobalVars
names, DeclareScript / Script names are *stored* qualified; every lookup (`ParseCommand` variables, `ParseCommands` script calls and globals)
tries the plain name first, then the qualified one, and `GSWorld::FindDeclIndexByName` (0x4D3780) strips a `!N!` prefix and retries. Module 0 (the
first compile after `Reset`: the base map's `SS_TEXT`) therefore has plain names; a quest map loaded on top, the generated quest chain and every
console line are later modules and see the base map's names, while the base map never sees theirs. Two modules may declare the same name without
a clash [V].

**Script calls** (`ParseCommands`): the declaration's signature string (`SSCScriptDecl +8`, letters in declaration order) is used as the parameter
list, with `scriptCall` mode reserving float slot 0 (= declaration index) and string slot 0 (= the name as written). The argument *count* per type
is not checked by the compiler beyond "not enough / too much" against that signature; the run-time `BindArguments` (0x4BB6A0) re-checks and
`FatalError`s on a mismatch, which can only happen with a stale save [I].

**`For` / `ForIf` / `Every` / `Any` / `Sum`**: signature `O g [f]`: the first argument must be a *GlobalVars object* variable (error 8 `Only
global variables are allowed to be loop counters`); the compiler stores a global binding for it, so at run time object slot 0 is the address of
that variable's cell and the loop writes it; the body / condition re-reads it through its own global bindings. The quest chain generator relies on
`GlobalVars ( i : object, Heroes : group )` existing in the map [V].

## 4. The compiled representation

Everything is MFC `CArray` / `CList` based; the records (all verified in the ctors / copy ctors / Read / Write, see `batch-script-structs.tsv`):

| Record | Size | Layout [V] |
|---|---|---|
| `SSCCmd` (one call) | 0x94 | `+0` function id; `+4` float slots (CArray: data +8, count +0xC); `+0x18` string slots (CStringArray: data +0x1C, count +0x20); `+0x2C` object slots (data +0x30: each slot = `ObjRef*`); `+0x40` group slots (data +0x44: each slot = `ObjRefCArray*`); `+0x54` local bindings; `+0x68` global bindings; `+0x7C` nested bindings (data +0x80, count +0x84); `+0x90` body `SSCProgram*` (For / ForIf) or 0 |
| `SSCCmdBinding` | 0xC | `{type 1..4, slot, varIndex}`: before each run `SetParameters` copies variable -> slot (floats and strings by value, objects / groups by *address*) |
| `SSCCmdNestedBinding` | 0xC | `{type 1/3/4, slot, SSCCmd*}`: `CalcParameters` evaluates the child into the slot (float value / temp `ObjRef*` / temp group) |
| `SSCProgram` | 0x18 | `{CArray<SSCCmd> commands (data +4, count +8), id +0x14}`; node of `GSWorld +0x70` (ids 0, 1, 2 ... = index) or `+0x178` (ids `0x80000000 \| index` = WorldScript bodies) |
| `SSCIfThenPair` | 8 | `{SSCProgram* condition, SSCProgram* body}` |
| `SSCScriptDecl` | 0xC0 | `+0` killed flag (active instances); `+4` name; `+8` signature letters (declaration order, one letter per `AddVariable`); `+0xC` pairs CArray (data +0x10, count +0x14; 0 = declared only); `+0x20` `SSCVars` parameters |
| `SSCVars` | 0xA0 | four `SSCVarBlock` (0x28 = values CArray 0x14 + names CStringArray 0x14): numbers `+0`, strings `+0x28`, objects `+0x50`, groups `+0x78` |
| `SSCScript` (a running frame) | 0xB4 | `+0` program, `+4` pc, `+8` run counter, `+0xC` sleep start (-1), `+0x10` kill flag pointer (or 0), `+0x14` `SSCVars` (a *copy* of the instance's parameters) |

Slot assignment (`Compiler_ParseCommand`): the i-th argument *of a given type* goes to slot i of that type's array (so `Attack(a, b)` with signature
`oo` uses object slots 0 and 1; `GSSetVar(f, s, f)` uses float 0, string 0, float 1). Literals are written into the slot at compile time; a
variable adds a binding `{type, slot, index}` (local list if the name is a parameter of the script, else global list); a nested call adds
`{type, slot, child}`. The number of slots is exactly the count of letters (plus the reserved ones): nothing is dynamic at run time.

Special encodings [V]:

| Statement | id | float slots | string slots | object / group slots | other |
|---|---|---|---|---|---|
| `Name(args)` script call | 8 | `[0]` = index of the declaration in `GSWorld +0x90`, then the `f` args | `[0]` = the name as written, then the `s` args | the `o` / `g` args from 0 | run: copy the declaration into the active list, `BindArguments` |
| `global = expr` | 0x78 | `[0]` = index in the matching `GSWorld +0xA0` table, `[1]` = type 0 float / 1 string / 2 object / 3 group, `[2]` = value when float | `[0]` = value when string | `[0]` = value when object / group | the value is a literal, a variable binding or a nested call bound to that slot |
| `For(var, grp) (body)` | 0x6B | - | - | object slot 0 = `&globalVar`, group slot 0 = the group | `+0x90` = body program |
| `ForIf(var, grp, cond) (body)` | 0x6C | `[0]` = cond (nested binding re-evaluated per element) | - | as For | `+0x90` = body |

The **symbol tables** are the name arrays of the `SSCVars`: the compiler looks a name up by linear `strcmp` in the names array of the type the
signature letter asks for (`SSCVarBlock_IndexOfName` 0x4D5A50..0x4D5B10), first the script's own `SSCVars` (declaration `+0x20`, or none for
`WorldScript` / console bodies), then `GSWorld +0xA0`. The same name may exist with several types; the letter decides. Scripts are found by name in
the declarations CArray `+0x90`; functions by the token id in the table. There is no constants pool: literals live in the slots, strings as CStrings.

**Error reporting** (`Compiler_SetError` 0x4CF470): `wvsprintfA(format, ...)` into `GSWorld +0x1EC`, `+0x1F0` = code, `+0x1F4` = line; the function
returns 0 and every parser returns that 0 upward, so a compile stops at the **first** error and `Compiler_CompileText` returns 0 with the VM left
holding whatever was appended before the failing top-level item (programs, declarations and globals of the earlier items stay; `+0x1A8` /
`+0x1AC` are *not* bumped). Who shows it: `App_LoadMapAndScript` (`FatalError "%s, line %i"` through `GSWorld::GetLastError`), the console
(`Console_RunScriptLine` 0x5DD800 prints `Script error: <text>`); `MobFile_Load` and `LoadWorldScript` ignore the result [V] (the map runs with the
partial script). The codes:

| code | message (format) | where |
|---|---|---|
| 1 | `Unexpected end of file`, `End of file before '*/'` | everywhere / tokenizer |
| 2 | `Wrong lexeme %s` | expected punctuation / `=` / `:` |
| 3 | `Unexpected '%s'` (top level), `Unexpected token '%s'` (statement), `Symbols behind ...` (dead entry points) | 0x4D23A0, 0x4D1330 |
| 4 | `Expected '%s'` (`(` or `,`), `Expected ',' or ')' after variable`, `If section, ( expected`, `then expected after if`, `Expected ( after then` | 0x4CFBC0, 0x4D1FE0, 0x4D1030 |
| 5 / 6 | `Not enough parameters` / `Too much parameters` | 0x4CFBC0 |
| 7 | `Parameter of type %s expected` (float / string / object / group) | 0x4CFBC0: wrong literal type, undeclared name, function of the wrong return type, a command as argument |
| 8 | `Only global variables are allowed to be loop counters` | `O` letter |
| 9 | `Function returning float expected` | conditions |
| 0xA / 0xB | `expected if section` / `In script %s should be at least one if section` | 0x4D1030 |
| 0xC / 0xD | `Unknown parameter type` / `Duplicate variable name %s` | 0x4D1FE0 |
| 0xE | `Only one WorldScript() per file allowed` | 0x4D23A0 |
| 0xF / 0x10 | `Duplicate script %s` / `Multiple declaration of script %s` | DeclareScript of a known name / Script body of a name that has one |
| 0x11 | `Unable to read file "%s"` | `Compiler_CompileFile` 0x4D36F0 |

Run-time `FatalError`s (message box, process ends): `KillScript command not allowed here`, `Wrong number of script %s float|string|object|group
parameters`, `Function does not return float|object|string at SSCCmd`, `SSCCmd::SetParameters, unknown parameter`, `SSCCmd::CalcParameters,
unknown function`, `wrong type char in CScript::LoadFunction`, `Uknown variable type at CScript..` [V].

**Dead compiler entry points** (0 callers, editor leftovers): `Compiler_DeclareScript` 0x4C01A0 (one `name(params)` text), `Compiler_CompileGlobalVars`
0x4BFCC0, `Compiler_CompileConditions` 0x4C06F0 / `Compiler_CompileCommands` 0x4C09F0 (one list into a program, error 3 `Symbols behind ... list`),
`Compiler_AddIfThenPair` 0x4C0520, and the **decompiler** `GSWorld::Decompile` 0x4D53C0 (`Decompiler_WriteVars` / `WriteProgram` / `WriteCommand`),
which rebuilds `GlobalVars ( ... ) DeclareScript N(...) Script N( if ... then ... ) WorldScript ( ... )` from the records and documents the canonical
layout: 2-space indents, `(` / `)` on their own lines, `ShitHappens` for an unresolvable slot; it recurses into `For` bodies but not `ForIf` ones.

## 5. GSWorld: the VM object (0x1F8)

| off | field | note [V] |
|---|---|---|
| +0 | vtable 0x73D394 | the point-light `CArray` vtable: GSWorld *starts with* `CArray<GSLightRecord>` (+4 data = 256 x 0x20, +8 count); slot 2 = `GSLights::Serialize` (unused) |
| +0x14 | particleSources CList (vt 0x73D388) | `GSEffectRecord` 0x24 `{obj, id, removeWhenDone, moving, steps, velocity[3], lifetime}` |
| +0x30 | soundSources CList (0x73D37C) | `GSSoundSourceRecord` 0x34 `{obj, id, CString wav, pos[3], volume, radius, randomized, scatter, minDelay, maxDelay, countdown}` |
| +0x4C | lightnings CList (0x73D370) | `GSLightningRecord` 8 `{obj, id}` |
| +0x68 / +0x6C | auto id counters | -2 after Reset; `CreateLightning` with a negative id uses and decrements +0x68 |
| +0x70 | programs CList (0x73D364) | every condition list / then body / For body: `SSCProgram`, id = index |
| +0x8C | declarations CArray (0x73D358) | `SSCScriptDecl[]`, data +0x90, count +0x94 |
| +0xA0 | globals `SSCVars` | numbers +0xA0, strings +0xC8, objects +0xF0, groups +0x118 |
| +0x140 | activeScripts CList (0x73D34C) | instances: `SSCScriptDecl` copies with bound arguments; **AddHead** on call |
| +0x15C | runningScripts CList (0x73D340) | `SSCScript` frames; **AddHead** (`StartScript`, `RunProgram`) |
| +0x178 | generatedPrograms CList (0x73D364) | the WorldScript body of every compile unit, ids `0x80000000 \| index` |
| +0x194 | areas CArray (0x73D334) | `GSArea` 0x18 `{zones CArray of GSAreaZone 0x14 {byte kind 0 circle / 1 rect, x, y, r^2 or x2, y2}, id}` |
| +0x1A8 | running | 1 after a successful compile; `Tick` is a no-op while 0 |
| +0x1AC | moduleId | compile unit counter (name prefix `!N!`) |
| +0x1B0 | tempGroups CList (0x73D43C) | `EvalGroup` results; one permanent (tail) node |
| +0x1CC | tempObjectRefs CList (0x73D448) | `EvalObject` results; one permanent node |
| +0x1E8 | forNesting | > 0 inside `For` / `ForIf` bodies: temporaries are kept |
| +0x1EC / +0x1F0 / +0x1F4 | error text / code / line | see section 4 |

`GSWorld::Reset` (0x4BE6E0: 15 callers, every zone load / unload / save load) empties *everything* and sets `+0x1AC` = 0; `ClearScriptLists`
(0x4BE490) empties only the active and running lists; `ResetAndRestart` (0x4BEB00, zone records without a script file) does that, removes the
effect objects and re-queues the first generated program (the WorldScript body) - a restart of the zone script with its globals kept [V].

## 6. Scheduling (`GSWorld::Tick` 0x4BF000, `SSCScript::Run` 0x4B6760, `GSWorld::RunProgram` 0x4BC420)

Per tick, in this order [V]:

1. **Running frames** (`+0x15C`, head first = most recently added first): `SSCScript::Run`. `Run` increments the run counter, returns 0 at once when
   `pc >= count` (the frame is then removed: a body that ends is dropped one tick after its last command), else purges dead references in the frame's
   vars and executes commands from `pc`: bind locals (`SetParameters` from `+0x14`) and globals (from the VM `+0xA0`), evaluate nested bindings
   (`CalcParameters`, depth first, left to right in binding order), run the case, `ClearTempResults`, `pc++` unless the command *stopped* the run
   (then `pc` stays and `Run` returns 1). A frame whose body never waits therefore runs to the end in one tick.
2. **Active instances** (`+0x140`, head first): `SSCVars::PurgeDeadObjects` on the instance's parameters, then for each `if/then` pair in source order
   `SSCIfThenPair::Run` (0x4BB520): the conditions are evaluated one by one (`EvalFloat`, `ClearTempResults` after each) and the first `0.0` ends the
   pair; if none is zero (an empty list counts as true) `RunProgram(body, instance)` builds a *stack* `SSCScript` {body, pc 0, counter 0, sleep -1,
   kill flag = &instance.killed, vars = a copy of the instance parameters} and runs it once; if it returns 1 (stopped in a wait before the end) a copy
   with **kill flag 0** is pushed to the running list. After each pair the instance's `killed` word is tested: set -> the instance is removed (the
   remaining pairs are skipped). Not set -> the next pair; **every true pair fires again next tick**, which is why bodies start with `KillScript()`.
3. Dead references in the global objects / groups are released (`ObjRef_ReleaseIfFlag1` / `ObjRefCArray_PurgeFlag1`: object flag bit 0).
4. Sound sources with `randomized == 0`: countdown, respawn a `CEffectSound` at `pos +- rand * scatter`, new countdown in `[minDelay, maxDelay]`.
5. Moving point lights: `steps--`, position += velocity, `CEffect::MarkDirty`; at 0 removed when `removeWhenDone`.
6. Particle sources: `lifetime--`, same motion; expired nodes removed (the list is freed when it empties).
7. `ClearTempResults`.

**Waits** (the only commands that return "stop"): `Sleep(n)` 0x7: `sleepStart = counter` at the first stop, stops while `counter - sleepStart <
round(n)`, then resets `sleepStart = -1` and continues (the command is re-run every tick, its argument re-evaluated); `WaitSegment(n)` 0x9: stops
while `counter < round(n)` (absolute, counted from the frame's start); `SleepUntil(expr)` 0xDE: stops while `expr == 0`; `SleepUntilIdle(unit)` 0xB9:
stops while the unit is a unit and its brain's current logic `+0x984` is non-zero. Since the counter is per frame and a frame runs once per tick,
`Sleep(n)` = n ticks of the VM that owns it [V].

**Script call** 0x8: `SSCScriptDeclCList_AddHead(+0x140, declarations[float slot 0])` + `BindArguments` (the arguments are copied *by value* into
the new instance: floats, strings, object cells (refcount++), groups (copied)); `killed = 0`; the caller continues. Calling a running script again
makes a second independent instance. Nothing is returned [V].

**`KillScript()`** 0x79: `*killFlag = 1` (FatalError when the frame has no flag: `WorldScript` body, console line, or a then-body that already
slept once and was re-queued with flag 0). The rest of the current body still runs [V].

**`For` / `ForIf`** 0x6B / 0x6C: `+0x1E8++`; the group is walked **from the last element to the first** (the group is pointer-sorted, so the order
is arbitrary but reversed w.r.t. `Every` / `Any` / `Sum`, which walk 0..n-1); per element the global loop variable cell is written
(`ObjRef_Release` + `ObjRef_Assign` through object slot 0), for `ForIf` the condition binding is re-evaluated and the body is skipped when it is 0,
then the body runs through a **recursive `SSCScript::Run` on the same frame** with `{program, pc, counter, sleepStart}` swapped to
`{body, 0, 0, -1}` and restored afterwards; the inner result is ignored, so a wait inside the body ends that iteration (the next element starts at
once) and a `Sleep` in a loop body never sleeps longer than one run. `+0x1E8--` at the end; temporaries made inside the loop live until then [V].

**Re-entrancy.** `Compiler_CompileText` may be called from inside `Run` (`QFinish` -> `QuestObjectives_GenerateScript`, a trap or a named event
compiling a `WorldScript(...)` into the *same* VM): the new programs / declarations / globals are appended and the new WorldScript body is added at
the head of the running list, where the outer `Tick` loop (which already took the next pointer) does not reach it until the next tick [V].
`Tick` is also called 5 times in a row by `PlayerIn` and `OnNamedEvent` (world-server.md) to let one-shot scripts finish before the player is shown.

## 7. Evaluation: the four switches

Every command / function is an `SSCCmd` executed by one of four switches on the function id. The arguments are always read from the slots in
signature order (float slot i = the i-th `f`, string slot i = the i-th `s`, ...); object slots are `ObjRef*` (deref once for the `CObject*`,
`ObjRef_AsUnit` 0x4B2530 for the unit check), group slots `ObjRefCArray*`. Results: `EvalFloat` writes `*out`; `EvalObject` fills a fresh temp
cell (`GSWorld::NewTempObjectRef` 0x4B59F0) and returns its address; `EvalGroup` fills a fresh temp group (`GroupCList_AddTail` on `+0x1B0`).
A string-returning function does not exist: a nested binding of type 2 is a FatalError (`Function does not return string`), hence no `S`
signatures in the table [V].

| switch | address | ids | result |
|---|---|---|---|
| `SSCScript::Run` (R) | 0x4B6760 | 137 cases: 2..0xE8 commands, 8 call, 0x78 assign; **no case = no-op** for 0x1 SetSunLight, 0x5 StartAnimation, 0x6 SetDirectionToObject, 0xA WaitEndAnimation, 0x13 SetCameraPosition, 0x14 SetCameraOrientation, 0x1C PlayFX, 0x21 ShowBitmap, 0xA3 ShowCredits, 0xA4 SetPlayerAggression | stop / continue |
| `SSCCmd::EvalFloat` (F) | 0x4B41E0 | all 68 `F` functions 0x45..0xE6 (0xA1 < id - 0x45 -> FatalError) | float |
| `SSCCmd::EvalObject` (O) | 0x4B56C0 | 0x4E GetObject, 0x85 GetObjectByName, 0xAB GetObjectByID, 0xB4 GetUnitOfPlayer, 0xE4 GetLeader | temp `ObjRef*` (null on a miss) |
| `SSCCmd::EvalGroup` (G) | 0x4B5B00 | 0x59 GroupAdd, 0x5A GroupSub, 0x5B GroupCross, 0x5D UnitSee, 0x5E GroupSee, 0x62 GetPlayerUnits, 0xA5 PlayerSee | temp group (pointer-sorted set) |

The full id table (every row = one `case`; S = switch; "slots" = where `SetParameters` / `CalcParameters` put the arguments; the behaviour of each
handler is in gs-functions.md section 3 and is not repeated):

| id | name | S | params | slots | result / note |
|---|---|---|---|---|---|
| 0x1 | SetSunLight | R (no case) | fffffffff | F0 F1 F2 F3 F4 F5 F6 F7 F8 | NO-OP in this build |
| 0x2 | MoveToPoint | R | off | O0 F0 F1 | continue |
| 0x3 | RotateTo | R | off | O0 F0 F1 | continue |
| 0x4 | MoveToObject | R | oo | O0 O1 | continue |
| 0x5 | StartAnimation | R (no case) | os | O0 S0 | NO-OP in this build |
| 0x6 | SetDirectionToObject | R (no case) | f | F0 | NO-OP in this build |
| 0x7 | Sleep | R | f | F0 | stop while counter - sleepStart < round(F0) |
| 0x8 | (script call) | R | decl sig | F0 = decl index, S0 = name, args after | AddHead active instance + BindArguments |
| 0x9 | WaitSegment | R | f | F0 | stop while counter < round(F0) |
| 0xA | WaitEndAnimation | R (no case) | - | - | NO-OP in this build |
| 0xB | CreatePointLight | R | ffffffff | F0 F1 F2 F3 F4 F5 F6 F7 | continue |
| 0xC | MovePointLight | R | ffffff | F0 F1 F2 F3 F4 F5 | continue |
| 0xD | DeletePointLight | R | f | F0 | continue |
| 0xE | CreateParticleSource | R | fffffs | F0 F1 F2 F3 F4 S0 | continue |
| 0xF | CreateLightning | R | ffffffff | F0 F1 F2 F3 F4 F5 F6 F7 | continue |
| 0x10 | MoveParticleSource | R | ffffff | F0 F1 F2 F3 F4 F5 | continue |
| 0x11 | DeleteParticleSource | R | f | F0 | continue |
| 0x12 | AttachParticleSource | R | ff | F0 F1 | continue |
| 0x13 | SetCameraPosition | R (no case) | fff | F0 F1 F2 | NO-OP in this build |
| 0x14 | SetCameraOrientation | R (no case) | f | F0 | NO-OP in this build |
| 0x16 | CreateFXSource | R | ffffffs | F0 F1 F2 F3 F4 F5 S0 | continue |
| 0x17 | CreateFX | R | fffffs | F0 F1 F2 F3 F4 S0 | continue |
| 0x18 | CreateRandomizedFXSource | R | fffffffffs | F0 F1 F2 F3 F4 F5 F6 F7 F8 S0 | continue |
| 0x19 | DeleteFXSource | R | f | F0 | continue |
| 0x1A | DeleteLightning | R | f | F0 | continue |
| 0x1B | PlayAnimation | R | os | O0 S0 | continue |
| 0x1C | PlayFX | R (no case) | ffffs | F0 F1 F2 F3 S0 | NO-OP in this build |
| 0x1D | PlayMusic | R | fs | F0 S0 | continue |
| 0x1E | SetWind | R | ffff | F0 F1 F2 F3 | continue |
| 0x1F | SetBackGroundColor | R | fff | F0 F1 F2 | continue |
| 0x21 | ShowBitmap | R (no case) | fs | F0 S0 | NO-OP in this build |
| 0x22 | AddObject | R | go | G0 O0 | continue |
| 0x23 | RemoveObject | R | go | G0 O0 | continue |
| 0x24 | UMClear | R | o | O0 | continue |
| 0x25 | UMSentry | R | off | O0 F0 F1 | continue |
| 0x26 | UMGuard | R | offf | O0 F0 F1 F2 | continue |
| 0x27 | UMGuardEx | R | offff | O0 F0 F1 F2 F3 | continue |
| 0x28 | UMPatrol | R | o | O0 | continue |
| 0x29 | UMPatrolClear | R | o | O0 | continue |
| 0x2A | UMPatrolAddPoint | R | off | O0 F0 F1 | continue |
| 0x2B | UMPatrolAddPointLook | R | offf | O0 F0 F1 F2 | continue |
| 0x2C | UMStandard | R | o | O0 | continue |
| 0x2D | UMAggression | R | o | O0 | continue |
| 0x2E | UMRevenge | R | o | O0 | continue |
| 0x2F | UMSuspection | R | o | O0 | continue |
| 0x30 | UMCorpseWatcher | R | o | O0 | continue |
| 0x31 | UMPlayer | R | o | O0 | continue |
| 0x32 | BlockUnit | R | of | O0 F0 | continue |
| 0x33 | Guard | R | offf | O0 F0 F1 F2 | continue |
| 0x34 | Idle | R | o | O0 | continue |
| 0x35 | KillUnit | R | o | O0 | continue |
| 0x36 | SetPlayer | R | of | O0 F0 | continue |
| 0x37 | SetDiplomacy | R | fff | F0 F1 F2 | continue |
| 0x38 | SetEnemy | R | of | O0 F0 | continue |
| 0x39 | SendEvent | R | ff | F0 F1 | continue |
| 0x3A | SendStringEvent | R | fs | F0 S0 | continue |
| 0x3B | Follow | R | oo | O0 O1 | continue |
| 0x3C | Attack | R | oo | O0 O1 | continue |
| 0x3D | Cast | R | off | O0 F0 F1 | continue |
| 0x3E | Crawl | R | o | O0 | continue |
| 0x3F | Stand | R | o | O0 | continue |
| 0x40 | Walk | R | o | O0 | continue |
| 0x41 | Run | R | o | O0 | continue |
| 0x42 | SetSpellAggression | R | of | O0 F0 | continue |
| 0x43 | InvokeAlarm | R | fff | F0 F1 F2 | continue |
| 0x44 | InflictDamage | R | of | O0 F0 | continue |
| 0x45 | IsEqual | F | ff | F0 F1 | float -> *out |
| 0x46 | IsLess | F | ff | F0 F1 | float -> *out |
| 0x47 | IsGreater | F | ff | F0 F1 | float -> *out |
| 0x48 | IsEqualString | F | ss | S0 S1 | float -> *out |
| 0x49 | Add | F | ff | F0 F1 | float -> *out |
| 0x4A | Sub | F | ff | F0 F1 | float -> *out |
| 0x4B | Mul | F | ff | F0 F1 | float -> *out |
| 0x4C | Div | F | ff | F0 F1 | float -> *out |
| 0x4D | Not | F | f | F0 | float -> *out |
| 0x4E | GetObject | O | f | F0 | temp ObjRef |
| 0x4F | HP | F | o | O0 | float -> *out |
| 0x50 | MaxHP | F | o | O0 | float -> *out |
| 0x51 | Mana | F | o | O0 | float -> *out |
| 0x52 | MaxMana | F | o | O0 | float -> *out |
| 0x53 | DistanceUnitUnit | F | oo | O0 O1 | float -> *out |
| 0x54 | DistanceUnitPoint | F | off | O0 F0 F1 | float -> *out |
| 0x55 | GetX | F | o | O0 | float -> *out |
| 0x56 | GetY | F | o | O0 | float -> *out |
| 0x57 | GetZ | F | o | O0 | float -> *out |
| 0x58 | GroupSize | F | g | G0 | float -> *out |
| 0x59 | GroupAdd | G | gg | G0 G1 | temp group |
| 0x5A | GroupSub | G | gg | G0 G1 | temp group |
| 0x5B | GroupCross | G | gg | G0 G1 | temp group |
| 0x5C | GroupHas | F | go | G0 O0 | float -> *out |
| 0x5D | UnitSee | G | o | O0 | temp group |
| 0x5E | GroupSee | G | g | G0 | temp group |
| 0x5F | Random | F | f | F0 | float -> *out |
| 0x60 | GetDiplomacy | F | ff | F0 F1 | float -> *out |
| 0x61 | IsEnemy | F | of | O0 F0 | float -> *out |
| 0x62 | GetPlayerUnits | G | f | F0 | temp group |
| 0x63 | IsAlive | F | o | O0 | float -> *out |
| 0x64 | IsDead | F | o | O0 | float -> *out |
| 0x65 | GetPlayer | F | o | O0 | float -> *out |
| 0x66 | GetObjectID | F | o | O0 | float -> *out |
| 0x67 | IsAlarm | F | f | F0 | float -> *out |
| 0x68 | Every | F | Ogf | O0* G0 F0 | float; loop over G0 writing O0*, re-evaluating F0 |
| 0x69 | Any | F | Ogf | O0* G0 F0 | float; loop over G0 writing O0*, re-evaluating F0 |
| 0x6A | Sum | F | Ogf | O0* G0 F0 | float; loop over G0 writing O0*, re-evaluating F0 |
| 0x6B | For | R | Og | O0* G0 | body +0x90 per element of G0 (last first), O0* = element |
| 0x6C | ForIf | R | Ogf | O0* G0 F0 | body +0x90 per element of G0 (last first), O0* = element |
| 0x6D | ( | lexer/parser | - | - | token / keyword, no run-time case |
| 0x6E | ) | lexer/parser | - | - | token / keyword, no run-time case |
| 0x6F | if | lexer/parser | - | - | token / keyword, no run-time case |
| 0x70 | then | lexer/parser | - | - | token / keyword, no run-time case |
| 0x71 | , | lexer/parser | - | - | token / keyword, no run-time case |
| 0x72 | = | lexer/parser | - | - | token / keyword, no run-time case |
| 0x73 | : | lexer/parser | - | - | token / keyword, no run-time case |
| 0x74 | WorldScript | lexer/parser | - | - | token / keyword, no run-time case |
| 0x75 | Script | lexer/parser | - | - | token / keyword, no run-time case |
| 0x76 | DeclareScript | lexer/parser | - | - | token / keyword, no run-time case |
| 0x77 | GlobalVars | lexer/parser | - | - | token / keyword, no run-time case |
| 0x78 | (global assign) | R | expr | F0 = var index, F1 = type 0..3, value in F2 / S0 / O0 / G0 | writes GSWorld +0xA0 |
| 0x79 | KillScript | R | - | - | *killFlag = 1 (FatalError if none) |
| 0x7C | HaveItem | F | ff | F0 F1 | float -> *out |
| 0x7D | ConsoleFloat | R | f | F0 | console stream g_0x7C55A8 |
| 0x7E | ConsoleString | R | s | S0 | console stream g_0x7C55A8 |
| 0x7F | GSSetVar | R | fsf | F0 S0 F1 | continue |
| 0x80 | GSGetVar | F | fs | F0 S0 | float -> *out |
| 0x82 | GSDelVar | R | fs | F0 S0 | continue |
| 0x83 | AddUnitUnderControl | R | fo | F0 O0 | continue |
| 0x84 | RemoveUnitFromControl | R | fo | F0 O0 | continue |
| 0x85 | GetObjectByName | O | s | S0 | temp ObjRef |
| 0x86 | AddUnitToParty | R | fss | F0 S0 S1 | continue |
| 0x87 | RemoveUnitFromParty | R | fs | F0 S0 | continue |
| 0x88 | AddUnitToServer | R | sssfff | S0 S1 S2 F0 F1 F2 | continue |
| 0x89 | RemoveUnitFromServer | R | o | O0 | continue |
| 0x8A | LeaveToZone | R | fsf | F0 S0 F1 | continue |
| 0x8B | AlarmTime | F | f | F0 | float -> *out |
| 0x8C | AlarmPosX | F | f | F0 | float -> *out |
| 0x8D | AlarmPosY | F | f | F0 | float -> *out |
| 0x8E | SetCP | R | offf | O0 F0 F1 F2 | continue |
| 0x8F | GetAIClass | F | o | O0 | float -> *out |
| 0x90 | GetLeverState | F | o | O0 | float -> *out |
| 0x91 | SwitchLeverState | R | of | O0 F0 | continue |
| 0x92 | GiveQuestItem | R | fs | F0 S0 | continue |
| 0x93 | EraseQuestItem | R | ff | F0 F1 | continue |
| 0x94 | Sentry | R | off | O0 F0 F1 | continue |
| 0x95 | ActivateTrap | R | of | O0 F0 | continue |
| 0x96 | RemoveObjectFromServer | R | o | O0 | continue |
| 0x97 | GodMode | R | ff | F0 F1 | continue |
| 0x98 | GSSetVarMax | R | fsf | F0 S0 F1 | continue |
| 0x99 | IsUnitInWater | F | o | O0 | float -> *out |
| 0x9A | CreateParty | R | fs | F0 S0 | continue |
| 0x9B | RemoveParty | R | fs | F0 S0 | continue |
| 0x9C | SetCurrentParty | R | fs | F0 S0 | continue |
| 0x9D | CopyStats | R | fss | F0 S0 S1 | continue |
| 0x9E | SwitchLeverStateEx | R | off | O0 F0 F1 | continue |
| 0x9F | QuestComplete | R | fs | F0 S0 | continue |
| 0xA0 | AddMob | R | s | S0 | continue |
| 0xA1 | PlayCamera | R | s | S0 | continue |
| 0xA2 | IsCameraPlaying | F | - | - | float -> *out |
| 0xA3 | ShowCredits | R (no case) | s | S0 | NO-OP in this build |
| 0xA4 | SetPlayerAggression | R (no case) | ff | F0 F1 | NO-OP in this build |
| 0xA5 | PlayerSee | G | f | F0 | temp group |
| 0xA6 | GiveItem | R | fs | F0 S0 | continue |
| 0xA7 | GiveMoney | R | ff | F0 F1 | continue |
| 0xA8 | CastSpellPoint | R | sffff | S0 F0 F1 F2 F3 | continue |
| 0xA9 | CastSpellUnit | R | sffo | S0 F0 F1 O0 | continue |
| 0xAA | IsNight | F | - | - | float -> *out |
| 0xAB | GetObjectByID | O | s | S0 | temp ObjRef |
| 0xAC | SetScience | R | offffff | O0 F0 F1 F2 F3 F4 F5 | continue |
| 0xAD | SetCPFast | R | offf | O0 F0 F1 F2 | continue |
| 0xAE | RemoveQuestItem | R | of | O0 F0 | continue |
| 0xAF | GiveUnitQuestItem | R | os | O0 S0 | continue |
| 0xB0 | Rest | R | o | O0 | continue |
| 0xB1 | IsPlayerInDanger | F | f | F0 | float -> *out |
| 0xB2 | IsPlayerInSafety | F | f | F0 | float -> *out |
| 0xB3 | GiveUnitSpell | R | os | O0 S0 | continue |
| 0xB4 | GetUnitOfPlayer | O | ff | F0 F1 | temp ObjRef |
| 0xB5 | GetWorldTime | F | - | - | float -> *out |
| 0xB6 | RunWorldTime | R | f | F0 | continue |
| 0xB7 | FixWorldTime | R | f | F0 | continue |
| 0xB8 | SetWaterLevel | R | fff | F0 F1 F2 | continue |
| 0xB9 | SleepUntilIdle | R | o | O0 | stop while O0 unit busy (+0x984) |
| 0xBA | SetParticleSourceSize | R | ff | F0 F1 | continue |
| 0xBB | IsInSquare | F | ffffff | F0 F1 F2 F3 F4 F5 | float -> *out |
| 0xBC | UnitInSquare | F | offff | O0 F0 F1 F2 F3 | float -> *out |
| 0xBD | GetZValue | F | ff | F0 F1 | float -> *out |
| 0xBE | GetBSZValue | F | ff | F0 F1 | float -> *out |
| 0xBF | GetFutureX | F | of | O0 F0 | float -> *out |
| 0xC0 | GetFutureY | F | of | O0 F0 | float -> *out |
| 0xC1 | PlayMovie | R | s | S0 | continue |
| 0xC2 | HideObject | R | of | O0 F0 | continue |
| 0xC3 | AddRectToArea | R | fffff | F0 F1 F2 F3 F4 | continue |
| 0xC4 | AddRoundToArea | R | ffff | F0 F1 F2 F3 | continue |
| 0xC5 | IsInArea | F | fff | F0 F1 F2 | float -> *out |
| 0xC6 | DeleteArea | R | f | F0 | continue |
| 0xC7 | UMFear | R | of | O0 F0 | continue |
| 0xC8 | UMAg | R | of | O0 F0 | continue |
| 0xC9 | Lie | R | o | O0 | continue |
| 0xCA | UMFollow | R | oo | O0 O1 | continue |
| 0xCB | CopyLoot | R | fss | F0 S0 S1 | continue |
| 0xCC | EnableLever | R | of | O0 F0 | continue |
| 0xCD | GiveSkill | R | osf | O0 S0 F0 | continue |
| 0xCE | GiveStrength | R | of | O0 F0 | continue |
| 0xCF | GiveDexterity | R | of | O0 F0 | continue |
| 0xD0 | GiveIntelligence | R | of | O0 F0 | continue |
| 0xD1 | GetMercsNumber | F | f | F0 | float -> *out |
| 0xD2 | RecalcMercBriefings | R | - | - | continue |
| 0xD3 | RedeployParty | R | f | F0 | continue |
| 0xD4 | CopyItems | R | fss | F0 S0 S1 | continue |
| 0xD5 | QStart | R | s | S0 | QuestObjectives_Reset |
| 0xD6 | QFinish | R | - | - | QuestObjectives_GenerateScript + compile |
| 0xD7 | QObjArea | R | f | F0 | QuestObjectives_Add(id, S0, F0) |
| 0xD8 | QObjSeeObject | R | s | S0 | QuestObjectives_Add(id, S0, F0) |
| 0xD9 | QObjSeeUnit | R | s | S0 | QuestObjectives_Add(id, S0, F0) |
| 0xDA | QObjKillUnit | R | s | S0 | QuestObjectives_Add(id, S0, F0) |
| 0xDB | QObjKillGroup | R | s | S0 | QuestObjectives_Add(id, S0, F0) |
| 0xDC | QObjUse | R | sf | S0 F0 | QuestObjectives_Add(id, S0, F0) |
| 0xDD | QObjGetItem | R | f | F0 | QuestObjectives_Add(id, S0, F0) |
| 0xDE | SleepUntil | R | f | F0 | stop while F0 == 0 |
| 0xDF | FixItems | R | - | - | continue |
| 0xE0 | AddLoot | R | fss | F0 S0 S1 | continue |
| 0xE1 | GetLootItemsCount | F | f | F0 | float -> *out |
| 0xE2 | GetMoney | F | f | F0 | float -> *out |
| 0xE3 | WasLooted | F | o | O0 | float -> *out |
| 0xE4 | GetLeader | O | - | - | temp ObjRef |
| 0xE5 | IsUnitVisible | F | o | O0 | float -> *out |
| 0xE6 | IsUnitBlocked | F | o | O0 | float -> *out |
| 0xE7 | AttachParticles | R | fo | F0 O0 | continue |
| 0xE8 | ResetTarget | R | o | O0 | continue |

(`O0*` = object slot 0 holds the *address of the global variable's cell*; `F`/`S`/`O`/`G` + n = slot n of that type.)

**Object references and groups.** A variable of type object is one `ObjRef` cell (a `CObject*`, refcount at object `+4`, flags `+8`, id `+0xC`).
`GetObject(n)` -> `CWorldServer::FindObjectById(round(n))`; `GetObjectByID("s")` -> `FindObjectById(atol(s))`; `GetObjectByName("s")` ->
`FindObjectById(String_HashCI(s))` (named objects are registered under the case-insensitive hash of their name, world-server.md `+0x12DD0`). A miss
leaves the temp cell null: the `none` value; every handler tests for null and does nothing. `GSWorld::PurgeDeadObjects` (0x4BED90, from
`CWorldServer::RemoveObject` / `UnloadZone` on both VMs) and the per-run purge release every cell whose object has flag bit 0, so a killed-and-removed
unit turns into `none` in all variables and vanishes from all groups [V]. A group is an `ObjRefCArray` (0x14: CArray of cells) kept **sorted by pointer
value without duplicates** (`ObjRefCArray_InsertSorted` 0x4B2EF0); `GroupAdd` / `GroupSub` / `GroupCross` are set union / difference / intersection
(0x4B2620 / 0x4B2920 / 0x4B2B60); `AddObject` / `RemoveObject` insert / remove in place; `GroupSize` = count; `GroupHas` = linear pointer compare.
Iteration order is the address order (reversed by `For`): not stable across runs or saves.

**Global variables and the quest state.** `GlobalVars` live in `GSWorld +0xA0` and are the only assignable variables; they are *per VM*, saved with the
zone, and shared by every module compiled into that VM (base map, quest map, generated quest chain, named-event scripts). They are **not** the
`q.*` quest variables: `GSSetVar / GSGetVar / GSSetVarMax / GSDelVar` (0x7F / 0x80 / 0x98 / 0x82) go to the player's `StringFloatMap` at
`CPlayer +0xC4` (`FindPlayerById(round(f))`, `NamedFloatMap_Get` 0x460AF0; hosted lobby: `LobbyServer::GetEntryValueByName`), which is saved with
the party and sent to the client with messages 0x81 / 0x82 (players.md) - the VM only reads and writes them through those four handlers [V].
`gtime` and `constr.current` are entries of the same map set by the engine, not by the VM.

**Console `@expr` path** [V]: `Console_RunScriptLine` (0x5DD800, behind the cheat switch, game-memory.md "Console") builds `WorldScript( <line> )`
(`#` -> `WorldScript( ConsoleFloat( <line> ))`, `$` -> `ConsoleString`) and calls `WorldScript_Execute` (0x5785D0): `Compiler_CompileText(g_0x7C29CC, text, 1)`
into the **console VM** (`CWorldServer +0x12A6C`) then one `GSWorld::Tick` of that VM, then `GetLastError` -> `Script error: %s`. Consequences: a
console line is a new module of the console VM (it can see that VM's earlier globals only, not the zone script's `GlobalVars`; `GetObject` /
`GetLeader` / `GSGetVar` work because they go to the world); every line leaves its program in `+0x178` until the next `Reset`; a `Sleep` in a console
line parks the frame and it continues on the following `CWorldServer::Update`s (the console VM is ticked unconditionally). `ConsoleFloat` prints with
`ostream << (double)` to `g_0x7C55A8`. Only `World_ScriptSpawnFlash` (0x578E30, the rain thunder `CreateFX` text) shares the console VM. `SpawnPartyHeroes` (0x58EF20), `OnNamedEvent` (0x592090: `WorldScript( GSSetVar( player, "name", 2 ) #OnBriefingComplete( player, "name" ) )`), `CTrapObject::Update` (0x529B80), `MobFile_Load` (0x481660) and `LoadWorldScript` (0x596350) compile into the **zone VM** `+0x12874` [V]: each is a new module there (`+0x1AC` ++), sees the zone script's declarations and globals by plain name, and its WorldScript body is queued at the head of the running list; `OnNamedEvent` and `PlayerIn` then tick that VM 5 times. A map that wants the briefing hook declares `#OnBriefingComplete( p : float, n : string )` itself; without it the text fails with error 3 and `OnNamedEvent` ignores the result, so the `GSSetVar` part is **not** executed either (the compile stops before `StartScript`) [V].

## 8. Persistence (`GSWorld::Write` 0x4C0F90 / `Read` 0x4C0D00; chunk 12 of the zone save, world-server.md section 5)

Only the zone VM `+0x12874` is written; the global `g_0x7AA620` points at the VM being (de)serialized and the readers of program ids are hard-wired
to `g_0x7C27D4` (`+0x74` / `+0x17C` list heads = `g_0x7C2848` / `g_0x7C2950`), so the console VM could not be loaded even if it were saved [V].
The stream (all ints 4 bytes, CStrings as length + 1 raw bytes, object references as the object id `+0xC` or -1 and re-resolved with
`CWorldServer::FindObjectById` on load, in this order):

1. point lights: count, `GSLightRecord_Write` each (object id, id, removeWhenDone, moving, steps, velocity);
2. particle sources: count + `GSEffectRecord_Write`; 3. sound sources: count + `GSSoundSourceRecord_Write` (incl. the wav name and countdown);
4. lightnings: count + `{object id, id}`; 5. `+0x68`, `+0x6C`;
6. compiled programs: count, then per program `SSCProgram_Write`: id, command count, `SSCCmd::Write` per command = id, float slots (count + raw),
   string slots (count + strings), object slot count, group slot count (the slot *contents* are not saved: they are re-bound at run time), local and
   global bindings (count + 0xC raw each), nested bindings (count, then `{type, slot}` + the child `SSCCmd::Write`, recursive), body program id or -1;
7. generated programs (same format); 8. declarations: count, then `SSCScriptDecl::Write`: killed word, name, signature, pair count + `{condition id, body id}`,
   the four variable tables (values with names); 9. the global `SSCVars` (numbers + names, strings + names, object ids + names, groups: count + ids);
10. active instances: count + `SSCScriptDecl::Write` (with the bound argument values); 11. running frames: count + `SSCScript::Write`: program id, pc,
    run counter, sleep start, index of the kill-flag owner in the active list (-1 = none), the frame's vars; 12. `+0x1A8`, `+0x1AC`;
13. areas (`GSAreas_Write`: count; id, zone count, 0x14-byte zones raw) - read back only when the stream version > 0x56.

So a save holds the *compiled* script, not the text: a changed `.mob` script is not picked up by an existing save of that zone (the loader
`LoadSave` reads the VM after `LoadZone` built the objects; the program ids are indices, so `Read` rebuilds the program lists in the same order).
Waits survive a save (pc and counters are stored), temporaries do not (they are per command). The two auto-id counters and the effect lists come
back with their objects re-linked by id. **Replication: none.** Nothing of `GSWorld` is sent to clients; scripts act on the server world and clients
see the ordinary object / effect replication (gs-functions.md section 1.2) [V].

## 9. What is not known

- ~~Whether a failed `.mob` compile is reported~~ settled 2026-11-03 [V]: it is not. The only readers of the error state
  (`+0x1EC` text / `+0x1F0` code, `GSWorld::GetLastError` 0x456360) are `App_LoadMapAndScript` (the editor path),
  `WorldScript_Execute` (the console) and `Compiler_CompileFile`. `MobFile_Load` 0x481660 and `SpawnPartyHeroes` call
  `Compiler_CompileText` and drop the result. A broken `.mob` script fails silently: the map runs with everything compiled before
  the first error and nothing after it (useful for mod authors: check scripts with the console or the multitool checker).
- ~~`+0x6C`~~ settled [V]: `nextAutoId2` is the id of script sound sources created without one: `GSWorld::CreateSoundSource` 0x4BD310 (CreateFXSource / CreateRandomizedFXSource) takes it when the given id is < 0 and decrements it (-2, -3, ...); reset by `GSWorld::Reset` 0x4BE6E0, saved.
- The MFC `Serialize` slots of all script containers (vt slot 2) are present but no `CArchive` path into them was found; the save uses the explicit
  `Write` / `Read` pairs. Taken as dead code.
- `GSWorld::Decompile` and the five `Compiler_*` one-list entry points have no callers: editor leftovers [untestable].
- The sound-source respawn inversion (gs-functions.md section 5): `Tick` serves records with `randomized == 0` [V]; whether randomized sources ever play is a [live] check.
- `GSPlayerLayer::dtor` (0x4A6840, was `CompiledScript_Destroy`, then `Destroy?`): the member destructor called from the `CGSPlayer` ctor / dtor EH funclets (guess sweep 2026-11-01); `CGSPlayer` / `GSPlayers_*`
  (0x4A6580..0x4A7630) are the session's player / shop records and have nothing to do with the script VM despite the "GS" prefix.

## 10. Corrections to earlier notes

| file | wrong statement | right one |
|---|---|---|
| gs-functions.md 1.1 (4) | "the text is kept in GSWorld+0x1EC with the line in +0x1F0 and the code in +0x1F4" | `+0x1F0` = code, `+0x1F4` = line (`Compiler_SetError`: the 2nd argument at every call site is the token line and lands in +0x1F4); `GetLastError` out = {text, code, line} |
| gs-functions.md 1.1 (2) | "`Compiler_Tokenize` splits ... `GSLexer_ClassifyToken` gives every token an id: ... the longest function name of the table that prefixes the text" | also *cuts the raw token after that name*: an identifier may not begin with a table name (`Sleepy` -> `Sleep` `y`); number literals are `atof`, not integers |
| gs-functions.md 1.2 | "a group is ... a set; iteration order is arbitrary" | arbitrary = pointer order; `For` / `ForIf` walk it last-to-first, `Every` / `Any` / `Sum` first-to-last |
| gs-functions.md 1.4 / world-server.md +0x12A6C row | the console VM runs "small generated effects (`World_ScriptSpawnFlash`)" (right) but world-server.md lists `SpawnPartyHeroes` / `OnNamedEvent` as its users | `SpawnPartyHeroes`, `OnNamedEvent`, `CTrapObject::Update`, `MobFile_Load`, `LoadWorldScript` compile into the zone VM `+0x12874`; only the console and `World_ScriptSpawnFlash` use `+0x12A6C` |
| gs-functions.md 5 | `GetBSZValue`: "the result is stored from the FPU stack although the callee returns a pointer in EAX; treat it as undefined" | resolved: the callee is `CWorldServer` vtable slot 28 = `GetTerrainCell` (returns the 9-byte cell pointer); the client vtable has `GetHeight` in slot 28, so the function reads a stale slot: undefined on the server, where all scripts run |
| gs-functions.md 3.1 / scripting.md | "`Script Name ( a : type, ... ) ( if ... )` for one that is defined without a declaration" (right) and scripting.md's `else` | there is no `else` and no `if` inside command lists (0x4D1330 has no such case) |
| scripting.md "Script validation", mob_script_check.hpp | "a group variable is created by its first use", "an undeclared name in an object position refers to a named object placed in the map" | both are compile error 7 in the engine (`Parameter of type group|object expected`); a map doing this has no working script at all. Named map objects are reached only through `GetObjectByName("...")` |
| scripting.md / mob_script_check.hpp | "a script call with the wrong argument count" is a warning | compile error 5 / 6 against the declaration's signature; the run-time `BindArguments` FatalError is unreachable from fresh text |
| mob_script_check.hpp | identifiers "may contain '#', '-' and '.'"; comments start with `//` | also: identifiers may not *begin* with any table name (case-insensitive prefix cut); `/* */` comments exist; `#define` lines exist and are textual |
| findings.md 0x4BB000 section | "Compiler = the GSWorld itself ..." (right); functions.tsv `Compiler_ParseCommands`: "nested if ( conditions ) then ( commands ) blocks (recursing into the nested program +0x90 of an 0x6b command)" | 0x6B / 0x6C are `For` / `ForIf`; there is no nested `if` |
| functions.tsv | `Decompiler_WriteProgram` "a command with the function id 0x6b (if)" | 0x6B = `For` |
| functions.tsv | `GSWorld::IsScriptDeclared` | searches the *active instance* list `+0x144`: renamed `GSWorld::IsScriptActive` |
| functions.tsv | `GSWorld::ResetPlayers_Single/Multi`, `CompiledScript_Destroy` | session / party-store functions, not the VM (renamed `Session_ResetPlayers_*`, `GSPlayerLayer::Destroy?`) |
| functions.tsv (CRT) | `atof` 0x6E911A, `CRT_isdigit` 0x6E9379, `CRT_isspace` 0x6E93CE | `atof` (`fld qword [eax+0x10]` of the `_fltin` result), `isdigit` (ctype bit 4), `isspace` (ctype bit 8) |
| functions.tsv | `Compiler_SetError` "+0x1f4 = code, +0x1f0 = line"; compile functions "-> 1 / 0 / error code" | reversed (see above); they return 0 on error, the code is in `+0x1F0` |
| functions.tsv | `Compiler_CompileText(this, text, qualify)` | the 3rd argument starts the WorldScript body (`StartScript`); qualification depends on `+0x1AC` only |
| game-memory.md "Console" | "`#Sin(0)` prints 0" | there is no `Sin` in the 225-entry table: `#Sin(0)` is compile error 7; "`@SetCameraPosition(57,65,20)` did nothing" is explained: 0x13 has no `Run` case (no-op), like the 9 other ids listed in section 7 |
| game-memory.md "Scripts and quests" | "Each variable is a heap object of class 0x0073BCE8" | those are the `StringFloatMap` nodes of `CPlayer +0xC4` (`q.*` quest values), not script variables; script `GlobalVars` are the `SSCVars` arrays at `GSWorld +0xA0` |
| mob-format.md | `SS_TEXT` magic "`2899242187` (`0xACD33E2B`)" | 2899242187 = `0xACCEECCB` (the scrambled script chunk; `0xACCEECCA` = plain text) |
| um_rig.cpp comment on 0x5785D0 | "out: {CString, line, code}" | out = {CString, code, line} |

## 11. Function index (address, name, one line)

| address | name | one line |
|---|---|---|
| 0x4D23A0 | Compiler_CompileText | #define pass, tokenize, top level (GlobalVars / DeclareScript / Script / WorldScript), module counter |
| 0x4CF7A0 | Compiler_Tokenize | raw tokens, comments, prefix cut |
| 0x4CF600 | GSLexer_ClassifyToken | literal / longest table-name prefix -> token id |
| 0x4D1FE0 | Compiler_ParseVarDecls | `name : type` lists |
| 0x4D1030 | Compiler_ParseIfThenPairs | `if ( ) then ( )` pairs of a Script |
| 0x4D0BF0 | Compiler_ParseConditions | F-function list |
| 0x4D1330 | Compiler_ParseCommands | statements: command / For body / script call / global assignment |
| 0x4CFBC0 | Compiler_ParseCommand | one call: slots, bindings, nested calls |
| 0x4D1D50 | Compiler_ParseGlobalVarAssign | `global = expr` -> id 0x78 |
| 0x4CF4D0 / 0x4CF470 | Compiler_ExpectToken / Compiler_SetError | punctuation check / error state |
| 0x4D37F0 / 0x4D38F0 / 0x4D3780 | Compiler_QualifyName / ParseModulePrefix / GSWorld::FindDeclIndexByName | `!N!` names |
| 0x4D33E0 / 0x4D34F0 | Compiler_ApplyDefines / DefineList_InsertByLength | #define |
| 0x4D0B10 / 0x4D0BA0 | GSFunctions_FindSignature / NameById | table lookups by id |
| 0x4D36F0 / 0x4D5880 | Compiler_CompileFile / Compiler_ReadFile | script file (editor path) |
| 0x4D53C0 / 0x4D50C0 / 0x4D41C0 / 0x4D39D0 | GSWorld::Decompile / Decompiler_WriteProgram / WriteCommand / WriteVars | dead decompiler |
| 0x4BDE40 / 0x4BE6E0 / 0x4BE490 / 0x4BEB00 | GSWorld::ctor / Reset / ClearScriptLists / ResetAndRestart | VM life cycle |
| 0x4BF540 / 0x4BF900 / 0x4BFA50 | GSWorld::AddProgram / AddProgramNeg / FindProgramById | program store |
| 0x4BCC60 / 0x4BC420 | GSWorld::StartScript / RunProgram | frames into the running list |
| 0x4BF000 | GSWorld::Tick | the scheduler |
| 0x4B6760 | SSCScript::Run | command switch (137 cases) |
| 0x4BB520 / 0x4BB6A0 / 0x4BB800 | SSCIfThenPair::Run / SSCScriptDecl::BindArguments / AddVariable | pairs, calls, declarations |
| 0x4B3F60 / 0x4B4030 | SSCCmd::SetParameters / CalcParameters | binding and nested evaluation |
| 0x4B41E0 / 0x4B56C0 / 0x4B5B00 | SSCCmd::EvalFloat / EvalObject / EvalGroup | value functions |
| 0x4BEC80 / 0x4BED90 / 0x4B59F0 | GSWorld::ClearTempResults / PurgeDeadObjects / NewTempObjectRef | temporaries, dead references |
| 0x4B2EF0 / 0x4B2620 / 0x4B2920 / 0x4B2B60 / 0x4B3110 | ObjRefCArray_InsertSorted / Union / Difference / Intersection / PurgeFlag1 | group set operations |
| 0x4B3940 / 0x4B36D0 / 0x4B37D0 / 0x4B3AD0 | SSCVars::PurgeDeadObjects / HasVariable / FindVariable / Read | variable tables |
| 0x4BCE10 / 0x4BCF30 / 0x4B2070 / 0x4B1ED0 | GSWorld::FindOrAddArea / DeleteArea / GSArea_AddCircle / AddRect | areas |
| 0x4BAA40 / 0x4BAE10 / 0x4BAAB0 | QuestObjectives_Reset / Add / GenerateScript | QStart / QObj* / QFinish |
| 0x4C0F90 / 0x4C0D00 | GSWorld::Write / Read | save chunk 12 |
| 0x4BB320 / 0x4BB070 / 0x4B6360 / 0x4B5FE0 / 0x4C1230 / 0x4BBFE0 | SSCScript / SSCCmd / SSCProgram / SSCScriptDecl Write + Read | record (de)serialization |
| 0x4CA810 / 0x4CA980 / 0x4CAC00 / 0x4CA000 / 0x4BB590 | SSCProgramCList_Read / SSCScriptDeclCList_Read / SSCScriptCList_Read / IfThenPairCArray_Read / SSCIfThenPair_Read | list loaders |
| 0x5785D0 / 0x5DD800 / 0x456360 | WorldScript_Execute / Console_RunScriptLine / GSWorld::GetLastError | console path |
| 0x596350 / 0x481660 / 0x592090 / 0x58EF20 / 0x578E30 / 0x529B80 | LoadWorldScript / MobFile_Load / OnNamedEvent / SpawnPartyHeroes / World_ScriptSpawnFlash / CTrapObject::Update | script text sources (world-server.md) |
