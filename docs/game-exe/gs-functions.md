# WorldScript (GS) function table and engine behaviour

Source: game.exe (Russian build, md5 6b6db6a4...) `g_0x78BCA0`, 225 records of 16 bytes {name*, id, signature*, params*}, dumped by `_cpr/claude-re/full/tools/gsfuncs.py`. The id is the switch case in `SSCScript::Run` (0x4B6760, commands), `SSCCmd::EvalFloat` (0x4B41E0, functions returning F), `SSCCmd::EvalObject` (0x4B56C0, O) and `SSCCmd::EvalGroup` (0x4B5B00, G). Params: f float, s string, o object, g group; a leading capital in the signature is the return type (F float, S string, O object, G group); commands have no return type.

This page was rewritten from the decompiles (`_cpr/claude-re/dec2/`) plus the disassembly of the switch tables; the handler addresses are the real case labels read from the jump tables, so they can be opened directly in a disassembler. User-facing syntax and examples stay in [scripting.md](../scripting.md) (the vendor reference); where the code disagrees with that page it is listed in section 6.

Status mark in the last column of every table:
- **V** verified: the handler itself was read in the decompile (and in the disassembly where the decompile was doubtful: argument order, switch tables, FPU code); the helpers it calls are described as their own functions.tsv / dec2 entries say - the shallow ones were read to the bottom, the big ones (path finding, AI logic updates, item handling) were not re-read.
- **I** inferred: the behaviour is read from code but the meaning of one field or helper is a reasoned guess (stated in the text).
- **G** guess / unreliable: the code does not allow a firm statement.

Nothing here was run in the game (the work was static); statements about "what the user sees" repeat earlier live checks from `docs/game-memory.md`.

## 1. How a script runs

### 1.1 Source, tokens, commands

Script text (the decrypted `SS_TEXT` node of a `.mob`, a generated quest script, or a console line) is compiled by `Compiler_CompileText` (0x4D23A0) into the VM object it is called on:

1. `#define name value` lines are collected and applied textually (longest name first, `Compiler_ApplyDefines`).
2. `Compiler_Tokenize` (0x4CF7A0) splits the text: whitespace and the separators `( ) , : =` end a token, `"..."` strings are kept whole, comments are `// to end of line` and `/* ... */`. `GSLexer_ClassifyToken` (0x4CF600) gives every token an id: 0x7A number literal, 0x7B string literal, otherwise the **longest function name of the table** that prefixes the text (so the ids 0x6D..0x77 below are the keywords and punctuation of the same table).
3. Top level: `GlobalVars ( name : type, ... )`, `DeclareScript Name ( a : type, ... )`, `Script Name ( if ( conditions ) then ( commands ) ... )` for a declared script (the shipped maps declare every script first), `Script Name ( a : type, ... ) ( if ... )` for one that is defined without a declaration, and `WorldScript ( commands )`. Types are `float`, `string`, `object`, `group`. An empty `if ( )` has no condition and is always true.
4. A command is parsed by `Compiler_ParseCommand` (0x4CFBC0): the name, `(`, one argument per letter of the signature, `)`. Wrong counts and types are compile errors ("Not enough parameters", "Parameter of type %s expected", "Function returning float expected", ...; the text is kept in GSWorld+0x1EC with the line in +0x1F0 and the code in +0x1F4). An argument is a literal, a variable name, or a nested call.

The result is a tree of **`SSCCmd`** records (0x94 bytes): `+0` function id; four typed slot arrays - floats (CArray header +4, data pointer +8), strings (+0x18 / +0x1C), objects (+0x2C / +0x30, each an `ObjRef*`), groups (+0x40 / +0x44) - the i-th argument of a type goes into the i-th slot of that type; `+0x54` and `+0x68` binding lists {type, destination slot, source variable index} for variables of the script itself and of GlobalVars; `+0x7C` nested calls {type, slot, SSCCmd*}; `+0x90` the body program of `For` / `ForIf`. Literals are stored in the slot at compile time.

Variables are named, typed and module-qualified: every successful compile bumps the module counter (GSWorld+0x1AC) and names get the prefix `!N!` (so the scripts and variables of two maps do not collide; when a name is looked up both its qualified and its plain form are tried, which is how a quest map reaches the scripts and variables of its base map).

There are two internal commands that are not in the table: id **0x8** (call of a user script) and id **0x78** (assignment to a GlobalVars variable), see the first table.

### 1.2 Evaluation

`SSCScript::Run` takes one command at a time. Before the switch it binds the variables into the slots (`SSCCmd::SetParameters`, both lists), evaluates the nested calls depth first (`SSCCmd::CalcParameters`: float -> `EvalFloat`, object -> `EvalObject`, group -> `EvalGroup`; a string-valued function does not exist - "Function does not return string" is a FatalError), then runs the case of the command id. The same happens again every time a waiting command is re-run, so conditions in `SleepUntil` are re-evaluated every tick.

- All numbers are float32; truth values are 0.0 / 1.0 (any non-zero is true); object ids are rounded with `_ftol`.
- An **object** is an `ObjRef` slot pointing at the game object (refcounted); a **group** is a CArray of such references kept **sorted by pointer, without duplicates** (a set; iteration order is arbitrary). References to objects that have been deleted (object flag 1) are released and nulled at the start of every `Run` and every `GSWorld::Tick` (`PurgeDeadObjects`), so a null reference is the normal "gone" value; nearly every command silently does nothing for a null object.
- Temporary groups made by `GroupAdd` / `UnitSee` ... live in a pool (GSWorld+0x1B0) that is emptied after each command (`ClearTempResults`) unless a `For` is active (nesting counter +0x1E8).
- All handlers act on the **server world** (`CWorldServer`, g_0x7AFF60); scripts run on the host (also in single player). Changes reach clients through the normal replication (`CMapObject::MarkNetDirty`, effect objects added with `CWorldServer::AddEffectObject`, or explicit net messages named in the entries). A few commands are purely local to the process (`PlayCamera`, `PlayMovie`, `SetBackGroundColor`, the console).

### 1.3 Frames, waiting, `if` / `then`

There is **no call stack**. A running script (`SSCScript`, 0xB4 bytes) is `{+0 program, +4 pc, +8 run counter, +0xC sleep start (-1), +0x10 pointer to the kill flag of its declaration or 0, +0x14 variables}`. `Run(script, vm)`: counter ++; returns 0 when `pc` is already past the end (the script is finished; this is why a script that ends in its first run is dropped one tick later), otherwise executes commands from `pc`; a waiting command leaves `pc` unchanged and ends the run (`Sleep`, `WaitSegment`, `SleepUntil`, `SleepUntilIdle`), any other command advances `pc`. Returns 1 when it stops or reaches the end.

Calling a script by name (id 0x8) does not run it: it adds a copy of the **declaration** to the VM's *active* list (GSWorld+0x140) and returns at once. Each tick `GSWorld::Tick` (0x4BF000), in this order:

1. runs every script of the running list (GSWorld+0x160; removed when `Run` returns 0);
2. for every active declaration: purges its dead references, then evaluates its `if(...)` condition lists in order - **all conditions of one pair must be non-zero (AND, short-circuit)**; a true pair runs its `then(...)` body through `GSWorld::RunProgram` (0x4BC420): a one-shot `SSCScript` with *copies* of the declaration's variables; if the body does not finish (it slept) a persistent copy is appended to the running list. The pairs are independent (there is no `else`); a true pair fires **every tick** until the instance is removed. `KillScript()` sets the instance's kill flag: the rest of that body still runs, then the pair loop ends and the instance is removed;
3. purges dead references in the global object / group variables;
4. advances the sound-source respawn timers, the point lights and the particle sources that `MoveParticleSource` / `MovePointLight` moved (position += velocity per tick; expiry).

This is why the generated quest scripts start every body with `KillScript()`, and why `KillScript` is an error in a `WorldScript` body, in a console line and in a body that already slept (RunProgram stores the suspended copy with a null kill pointer).

`For` / `ForIf` run their body by calling `Run` recursively on the same `SSCScript` with program, pc, counter and sleep state swapped out and restored; the inner result is ignored.

### 1.4 Who starts scripts (the VMs)

There are two `GSWorld` script VMs inside `CWorldServer` (g_0x7AFF60), both ticked from `CWorldServer::Update` (0x58DCD0):

| VM | Address | Used for |
|---|---|---|
| mission VM | world +0x12874 = g_0x7C27D4 | the map's mission script and everything the game compiles for quests (ticked once `CWorldServer+0x160` is non-zero or when the update is forced; `World::PlayerIn` sets +0x160 = 1 when a player enters a zone and ticks the VM 5 times at once) |
| console VM | world +0x12A6C = g_0x7C29CC | console lines (`@`, `#`, `$` prefixes through `WorldScript_Execute`, which compiles and ticks at once) and small generated effects (`World_ScriptSpawnFlash`); ticked every update |

Sources of script text for the mission VM: `CWorldServer::LoadWorldScript` (0x596350: the encoded script file, resets the VM first) and `MobFile_Load` (0x481660: the `SS_TEXT` of a loaded `.mob`; also `App_LoadMapAndScript` for the start map); the generated quest chain (`QFinish`, section 3.8); `CWorldServer::SpawnPartyHeroes` (0x58EF20: builds a `WorldScript( AddObject( Heroes, GetObjectByID( "id" ) ) PlayAnimation(...) )` text that fills the `Heroes` group when a party is placed); `Script::NamedEventSource` (0x592090: a named event (three dot-separated fields, e.g. a finished briefing) hands out / removes items and quest variables of an event table and then compiles `WorldScript( GSSetVar( player, "name", 2 ) #OnBriefingComplete( player, "name" ) )` - so a map can define a script called `#OnBriefingComplete` (identifiers may contain `#`) as the hook - followed by 5 extra ticks when the world mode is 3); `CTrapObject::Update` (a trap that fires compiles `Worldscript( CreateParticleSource( 100, x, y, z, 1, "Mushroom"))`). After a compile the VM's "running" flag (GSWorld+0x1A8) is set and the module counter incremented; the five extra ticks of `World::PlayerIn` let the spawn scripts of the party finish before the player is shown.

### 1.5 Where variables live

| Kind | Declared by | Storage | Lifetime / saving |
|---|---|---|---|
| global script variables | `GlobalVars (...)` | the VM's `SSCVars` at GSWorld+0xA0 (float / string / object / group tables with name tables) | assigned with `name = expr` (id 0x78); saved with the world (`GSWorld::Write`, `SSCVars::Write*`); object and group values are stored as object ids and re-resolved on load |
| script parameters / locals | `Script Name (a : float, g : group ...)` | the declaration (`SSCScriptDecl` 0xC0 bytes: +0x20 floats, +0x48 strings, +0x70 objects, +0x98 groups, each table followed 0x14 bytes later by its name array); copied into the active instance by `BindArguments`, into the running script by `RunProgram` | per instance; **cannot be assigned** (only GlobalVars can) - the loop variable of `For` is the only thing that changes them |
| `Heroes` | a global group of the mission script | filled by the spawn script of `SpawnPartyHeroes` | as a global |
| persistent named numbers | `GSSetVar` / `GSSetVarMax` / `GSDelVar` / `GSGetVar` | per player: `StringFloatMap` at player +0xC4 (setting 0 deletes), sent to the client with net opcode 0x81 / 0x82; in a hosted lobby one session-wide table in `LobbyServer` | this is the **quest state** (`q.<quest>.<quest>[.<n>]`); saved with the party / session, not with the script VM |
| objectives | `QStart` / `QObj*` | global table g_0x7AA608 of {command id, text, number} | cleared by `QStart`; consumed by `QFinish` |

Areas (`AddRoundToArea`, `AddRectToArea`) are records of the VM (GSWorld+0x198) keyed by a number. Lights, particle sources, lightnings and sound sources are effect objects tracked in GSWorld lists (+0x4 / +0x14 / +0x30 / +0x4C) so that scripts can move and delete them by id.

## 2. Reading the tables

- **id**: the function id. **params**: signature letters (f s o g), return type in the kind column.
- **Handler**: the address of the case label inside `SSCScript::Run` (R), `SSCCmd::EvalFloat` (F), `EvalObject` (O), `EvalGroup` (G); `Run default` = the shared no-op label 0x4B9BC5 (the id is accepted by the compiler but does nothing). Helpers called by a handler are named in the text with their addresses.
- Type ids used below: object type 0x50 = unit, 0x5D = lever, 0x14 = (quest) item class; flag bits are object flags (+8).
- "Server only" is the rule for all entries (section 1.2); only exceptions are mentioned.


## 3. The commands by theme

### 3.1 Language keywords and internal commands

| id | name | kind | params | handler | behaviour | st |
|---|---|---|---|---|---|---|
| 0x6D | `(` | syntax |  | compiler | Token `(`. Opens an argument list, a variable declaration list, an `if` / `then` section. No runtime code. | V |
| 0x6E | `)` | syntax |  | compiler | Token `)`. Ends a command list (Compiler_ParseCommands returns on it), an argument list or a section. | V |
| 0x71 | `,` | syntax |  | compiler | Token `,`. Separates arguments, statements and variable declarations (optional after a statement). | V |
| 0x73 | `:` | syntax |  | compiler | Token `:` in `name : type` declarations (types `float`, `string`, `object`, `group`; error 0xC "Unknown parameter type"). | V |
| 0x72 | `=` | syntax |  | compiler | Token `=` in `variable = expression`. Only GlobalVars variables may be assigned (compiled to the internal id 0x78). | V |
| 0x6F | if | syntax |  | compiler | Token `if`. Valid only as the head of an if/then pair directly inside a `Script` body (Compiler_ParseIfThenPairs). Not accepted inside a command list (the statement parser has no `if` / `else`: "Unexpected token"). | I |
| 0x70 | then | syntax |  | compiler | Token `then`. Follows the `if ( conditions )` part; its `( commands )` part is the body program. | V |
| 0x74 | WorldScript | syntax |  | Compiler_CompileText 0x4D23A0 | Top-level keyword `WorldScript ( commands )`: ONE per compile unit (error 0xE "Only one WorldScript() per file allowed"). The body becomes a generated program (negative id, GSWorld::AddProgramNeg) that GSWorld::StartScript queues as a running script; it runs from the next GSWorld::Tick, once, and may Sleep. It has no declaration, so KillScript is not allowed in it. | V |
| 0x75 | Script | syntax |  | Compiler_CompileText 0x4D23A0 | Top-level keyword `Script Name ( if(...) then(...) ... )`: defines the body of a script = 1 or more independent if/then pairs (error 0xB "In script %s should be at least one if section"; error 0x10 "Multiple declaration of script %s" when the name already has a body). After `Script Name (` the compiler either finds an earlier DeclareScript and reads the pairs directly (the form used by the shipped maps, which declare everything first) or, for an undeclared name, reads `a : type, ...  )  (` first. It is DORMANT until another command calls it by name (internal id 0x8); a call makes an active instance whose pairs are tested every tick. (functions.tsv text for Compiler_CompileText has the ids 0x75 / 0x76 swapped.) | V |
| 0x76 | DeclareScript | syntax |  | Compiler_CompileText 0x4D23A0 | Top-level keyword `DeclareScript Name ( a : type, ... )`: declares a script name and its parameter types without a body (error 0xF "Duplicate script" when the name exists), so that scripts can call each other before their definitions; the later `Script Name ( if ... )` fills it in. | V |
| 0x77 | GlobalVars | syntax |  | Compiler_CompileText 0x4D23A0 | Top-level keyword `GlobalVars ( name : type, ... )`: declares the VM-wide variables (stored in GSWorld+0xA0, module-qualified names). Only these can be assigned with `=`. | V |
| 0x7A | (number literal) | internal |  | tokenizer 0x4CF600 | Internal token id of a number literal (never in the table). The value is stored straight into the float slot of the command it belongs to. | V |
| 0x7B | (string literal) | internal |  | tokenizer 0x4CF600 | Internal token id of a string literal in double quotes (string slot). | V |
| 0x8 | (script call) | internal |  | Run 0x4B68F6 | INTERNAL, not in the table: a call of a user script `Name(args)`. Float slot 0 = index of the declaration in GSWorld+0x90 (0xC0-byte SSCScriptDecl), string slot 0 = the name, further slots = the arguments (BindArguments checks the counts: FatalError "Wrong number of script %s float / string / object / group parameters"). Run: SSCScriptDeclCList_AddHead(GSWorld+0x140, decl) copies the declaration into the ACTIVE list and binds the arguments; the caller continues at once (no stack frame, nothing is returned). Calling a script that is already active adds a second instance. | V |
| 0x78 | (set global variable) | internal |  | Run 0x4B6B3C | INTERNAL, not in the table: `globalVar = expression` (built by Compiler_ParseGlobalVarRef). Float slot 0 = index of the variable in the GSWorld global SSCVars (+0xA0), float slot 1 = type code (0 float, 1 string, 2 object, 3 group); the value is the nested expression result (float slot 2 / string slot 0 / object slot 0 / group slot 0; strings are CString-assigned, objects ObjRef_Assign'ed, groups copied with ObjRefCArray_Assign). Verified in the disassembly (0x4B6B3C: the type is read from slot 1, the index from slot 0). | V |

### 3.2 Flow control, waiting and loops

| id | name | kind | params | handler | behaviour | st |
|---|---|---|---|---|---|---|
| 0x79 | KillScript | command |  | Run 0x4B6932 | KillScript(): `*(SSCScript+0x10) = 1`. That pointer is the kill flag of the ACTIVE script instance the body belongs to (set by RunProgram); GSWorld::Tick removes the instance after the pair loop. The rest of the current body still runs. FatalError "KillScript command not allowed here" when the pointer is 0: in a WorldScript() body, in the console, and in a body that has already slept (RunProgram clears the pointer before it stores the suspended copy). Inside a For / ForIf body it works (the body shares the pointer). | V |
| 0x7 | Sleep | command | f | Run 0x4B68A8 | Sleep(n): waits n GSWorld ticks (server world ticks, 1/15 s per the docs). First visit stores the run counter in SSCScript+0xC; while `counter - start < round(n)` the command stays current (pc not advanced); afterwards +0xC = -1 and the script goes on. Each Run call is one tick, so the arguments are re-evaluated every tick. | V |
| 0x9 | WaitSegment | command | f | Run 0x4B6874 | WaitSegment(n): waits until the script's own run counter (SSCScript+8, counts Run calls since it started) has reached n: an ABSOLUTE wait ("until segment n"), unlike Sleep which is relative to the call. | V |
| 0xDE | SleepUntil | command | f | Run 0x4B6C58 | SleepUntil(cond): the command is re-executed every tick (its nested condition is evaluated again) and stays current while the value is 0. | V |
| 0xB9 | SleepUntilIdle | command | o | Run 0x4B6C19 | SleepUntilIdle(unit): stays current while the object is a unit whose brain order kind (CUnitServer+0x980+4) is not 0, i.e. until its order queue is empty. A null / non-unit argument does not wait. | V |
| 0xA | WaitEndAnimation | command |  | Run default 0x4B9BC5 (no-op) | WaitEndAnimation(): no-op (default case of the Run switch). | V |
| 0x6B | For | command | Og | Run 0x4B6954 | For(var, group) ( body ): ctx nesting counter +0x1E8 ++; for every member of the group, from the LAST array index to the first (the group is a set sorted by object pointer, so the order is arbitrary but safe against RemoveObject), the loop variable (object slot 0) is set and the body program (SSCCmd+0x90, parsed with Compiler_ParseCommands) is run by a recursive SSCScript::Run on the same SSCScript with pc / counter / sleep state swapped and restored afterwards. The inner Run's result is ignored: a Sleep / SleepUntil inside the body only cuts the REST OF THAT ITERATION short, it does not suspend the outer script (read from code, not tested). | V |
| 0x6C | ForIf | command | Ogf | Run 0x4B6A0C | ForIf(var, group, cond) ( body ): as For, but the condition (nested float expression, recalculated per member with CalcParameters) must be non-zero for the body to run. | V |
| 0x68 | Every | function -> F | Ogf | EvalFloat 0x4B4C55 | Every(var, group, cond): 1.0 when `cond` is non-zero for every member (an empty group gives 1.0); stops at the first zero. The condition is the nested float binding of the command and is recalculated per member with the loop variable set; without such a binding the plain float argument is returned. | V |
| 0x69 | Any | function -> F | Ogf | EvalFloat 0x4B4D4C | Any(var, group, cond): 1.0 when `cond` is non-zero for at least one member (empty group: 0.0); stops at the first non-zero. | V |
| 0x6A | Sum | function -> F | Ogf | EvalFloat 0x4B4E54 | Sum(var, group, expr): the sum of `expr` over the members (empty group: 0.0). Without a nested expression the result is count * the plain float argument. | V |

### 3.3 Math and logic

| id | name | kind | params | handler | behaviour | st |
|---|---|---|---|---|---|---|
| 0x45 | IsEqual | function -> F | ff | EvalFloat 0x4B4279 | IsEqual(a, b): 1.0 when a == b (exact float compare) else 0.0. | V |
| 0x46 | IsLess | function -> F | ff | EvalFloat 0x4B42B1 | IsLess(a, b): a < b. | V |
| 0x47 | IsGreater | function -> F | ff | EvalFloat 0x4B42E9 | IsGreater(a, b): b < a. | V |
| 0x48 | IsEqualString | function -> F | ss | EvalFloat 0x4B4321 | IsEqualString(s1, s2): strcmp == 0 (case sensitive). | V |
| 0x49 | Add | function -> F | ff | EvalFloat 0x4B435C | Add(a, b): a + b. | V |
| 0x4A | Sub | function -> F | ff | EvalFloat 0x4B437C | Sub(a, b): a - b. | V |
| 0x4B | Mul | function -> F | ff | EvalFloat 0x4B439C | Mul(a, b): a * b. | V |
| 0x4C | Div | function -> F | ff | EvalFloat 0x4B43BC | Div(a, b): a / b, no zero check (IEEE: inf / nan). | V |
| 0x4D | Not | function -> F | f | EvalFloat 0x4B43DC | Not(a): 1.0 when a == 0.0 else 0.0. | V |
| 0x5F | Random | function -> F | f | EvalFloat 0x4B481A | Random(n): a float in [0, n): the next 32-bit word of the game's ISAAC generator (g_0x7A9D40, shared with the AI) * n * 2^-32. | V |

### 3.4 Unit orders, stance and AI behaviours

| id | name | kind | params | handler | behaviour | st |
|---|---|---|---|---|---|---|
| 0x2 | MoveToPoint | command | off | Run 0x4B71B2 | MoveToPoint(unit, x, y): when the object is a unit with a CUnitServer: Brain::OrderMoveTo (0x5D7AE0 `Order_MoveTo`) = brain order kind 1 + CUnitServer::QueueOrderMoveTo((x, y, 0), run 0, 0, z = 1e6 = "ground"). The queued order replaces the pending one; the unit walks in its current stance. Units whose AI keeps overriding (e.g. villagers with their own logic) can walk back (observed by the user for an AI-class-3 villager: nothing happened). | V |
| 0x4 | MoveToObject | command | oo | Run 0x4B7267 | MoveToObject(unit, target): like MoveToPoint to the target's position at this moment (target +0x1C / +0x20); it does not follow. Both must be non-null. | V |
| 0x3 | RotateTo | command | off | Run 0x4B71FF | RotateTo(unit, x, y): Order_MoveTo2 (0x5D7B30) = brain order kind 1 + CUnitServer::QueueOrderMoveTo2 (0x5546B0), i.e. a queued unit order of kind 2 ("direct move", dispatched with 0x5B1DE0 + SetState(1) in DispatchQueuedOrder) whose position argument is the DISPLACEMENT (x - unit.x, y - unit.y, -unit.z). The effect is that the unit turns to face the point; the exact mechanics of the kind-2 order are not fully read. | I |
| 0x3B | Follow | command | oo | Run 0x4B7450 | Follow(unit, target): Brain::OrderFollow = brain order kind 6 on the target (refcounted into brain +0x2C); Brain::TickFollow keeps walking after it as long as the order lives. A null target is stored too (the order is dropped by CheckOrderTarget). | V |
| 0x3C | Attack | command | oo | Run 0x4B7489 | Attack(unit, target): Brain::OrderAttack = brain order kind 3 + CUnitServer::QueueOrderAttack(target, hit location 6 = any, ...); nothing when the target is null. | V |
| 0x3D | Cast | command | off | Run 0x4B74FA | Cast(unit, x, y): Brain::OrderCastAt(spell id 0, pos = (x, y, terrain height), flag 0): a "cast at point" order (brain kind 4) with spell id 0, i.e. whatever the unit has in its active spell slot (the id 0 is passed verbatim; CUnitServer::QueueCastAt resolves it). | I |
| 0x3E | Crawl | command | o | Run 0x4B7393 | Crawl(unit): CUnitServer::SetAttackMode(1) = stance 1 (crouch / sneak). Stance numbers of the script commands: Lie 0, Crawl 1, Stand = Walk 2, Run 3. SetAttackMode stores the wish in unit +0x268 and applies it if StanceAllowedHere (the passability map must allow the new posture); mode 3 is refused when the body is too damaged (UnitStats_EffectLevel). | V |
| 0x3F | Stand | command | o | Run 0x4B73C1 | Stand(unit): same code as Walk: SetAttackMode(2). | V |
| 0x40 | Walk | command | o | Run 0x4B73F6 | Walk(unit): SetAttackMode(2). | V |
| 0x41 | Run | command | o | Run 0x4B73C8 | Run(unit): SetAttackMode(3). | V |
| 0xC9 | Lie | command | o | Run 0x4B7365 | Lie(unit): SetAttackMode(0). | V |
| 0x34 | Idle | command | o | Run 0x4B7694 | Idle(unit): Brain::ClearLogics (drops every AI behaviour, current logic reset to the idle thresholds) + Brain::ClearOrder (order kind 0, target released): the unit stands and does nothing, and nothing makes it react until a new behaviour is added. | V |
| 0xB0 | Rest | command | o | Run 0x4B7424 | Rest(unit): CUnitServer::QueueOrderKindB = queued order kind 0xB (sit down / rest, DispatchQueuedOrder sends it through ApplyPendingStance: stance 4) unless the current order already is 0xB. | V |
| 0x1B | PlayAnimation | command | os | Run 0x4B7333 | PlayAnimation(unit, name): CUnitServer::QueueScriptOrder: looks the owner's model name up in the world's animation database (world vt[19]) and finds the entry called `name` (AnimDb_FindEntryByName); when found the queued order becomes kind 7 (kind 8 when entry flag 0x80000000 is set) with +0x1D0 = entry +0x10 (the animation); unknown name: nothing. The unit stops what it does while the animation plays (queued orders replace the pending one). | V |
| 0x5 | StartAnimation | command | os | Run default 0x4B9BC5 (no-op) | StartAnimation(unit, path): no-op (default case; the compiler accepts the call). | V |
| 0x35 | KillUnit | command | o | Run 0x4B7181 | KillUnit(unit): builds a hit record with Order_Init (0x522B40; hit[8] and hit[9] = 3.4e36, hit location 6 = any) and calls the object's vt[31] (forward to CUnitServer::TakeHit 0x553F40) with NO attacker. ResolveHitChance runs (hit[8] is the to-hit slot, overwritten with +-FLT_MAX; with no attacker the chance is FLT_MAX, so the hit always lands), then UnitStats_ApplyHit applies the huge damage. No attacker means no kill experience (TakeHit credits XP only when an attacker with a player is passed) and no aggro-table entry. | V |
| 0x44 | InflictDamage | command | of | Run 0x4B8133 | InflictDamage(unit, n): same path as KillUnit with hit[8] = 100.0 (overwritten by ResolveHitChance: always a hit without attacker), hit[9] = n (damage), hit location 1, backstab power 0, no attacker: CUnitServer::TakeHit -> UnitStats_ApplyHit (whether armour / resistances reduce n there was not read). The vendor text says XP is granted; with the attacker argument 0 the code does not give any (verified: both extra TakeHit arguments are 0). | I |
| 0x32 | BlockUnit | command | of | Run 0x4B72E5 | BlockUnit(unit, flag): only for type 0x50 units: flag non-zero sets object flag 0x800000, zero clears it (read back by IsUnitBlocked). Not marked net-dirty here. | V |
| 0xC2 | HideObject | command | of | Run 0x4B9ABA | HideObject(obj, flag): any non-null object: SetFlags / ClearFlags(0x10000) on its flags (+8), no type check and no net-dirty call in the handler; the flag is what the renderer / briefing zones use to hide the object. | I |
| 0x8E | SetCP | command | offf | Run 0x4B8CF8 | SetCP(obj, x, y, z): moves ANY map object: CollisionMap::MoveUnit(obj, (x,y,z)). Units: CUnitServer::ResetOrdersAndStance (orders and queued order cleared) and MarkNetDirty(2) when z changed. Non-units: after the move the units standing within 2 * +0x134 of the object are taken out of the collision map and put back one by one; a unit that has no passable neighbour cell is relocated to the nearest reachable cell (CAIMap::FindNearestReachableCell) and reset (ResetOrdersAndStance); the object is marked net-dirty (2). The z handed over is the object's own height offset above the terrain (+0x24). | V |
| 0xAD | SetCPFast | command | offf | Run 0x4B8EF2 | SetCPFast(obj, x, y, z): as SetCP without the neighbour handling: exchanges the byte at +2 of the passability map (world +0x170, SSCScript::SwapField2) to 0, CollisionMap::MoveUnit, ResetOrdersAndStance for units, byte back to 1, MarkNetDirty(2) always. | V |
| 0x33 | Guard | command | offf | Run 0x4B76CD | Guard(unit, x, y, radius): Brain::SetupGuardLogics: clears all behaviours and the current logic, adds AiLogicGuard(pos, radius, default stay time) + GoTo + Combat + kind 5 + AlertGuard(0); clears target and order. A COMPLETE replacement of the unit's AI (UMGuard only adds the guard logic). | V |
| 0x94 | Sentry | command | off | Run 0x4B7726 | Sentry(unit, x, y): Brain::SetupSentryLogics: as Guard with AiLogicSentry(pos) (stays on the exact point). | V |
| 0xE8 | ResetTarget | command | o | Run 0x4B74CA | ResetTarget(unit): Brain::ResetLogic = AiMemory::Clear + rebuild the logic objects from the unit's .mob logic slot (Brain::BuildFromLogicSlot, 0x5D6630): the unit returns to its map-defined behaviour. Call before building a custom UM* list. | V |
| 0x24 | UMClear | command | o | Run 0x4B777B | UMClear(unit): Brain::ClearLogics only (the current order keeps running). | V |
| 0x25 | UMSentry | command | off | Run 0x4B77AB | UMSentry(unit, x, y): adds an AiLogicSentry (class 0x73D190, kind 2) at (x, y). Brain::AddLogic replaces any logic of the same kind. | V |
| 0x26 | UMGuard | command | offf | Run 0x4B781F | UMGuard(unit, x, y, radius): adds an AiLogicGuard (kind 0, GuardPoint_Init with stay time -1 = the aiinfo default). | V |
| 0x27 | UMGuardEx | command | offff | Run 0x4B78A4 | UMGuardEx(unit, x, y, radius, delay): as UMGuard with the stay time `delay` at the stopping points. | V |
| 0x28 | UMPatrol | command | o | Run 0x4B798C | UMPatrol(unit): adds an empty AiLogicPatrol (kind 1, 0x20 bytes, vtable 0x73FF40); points come from UMPatrolAddPoint. | V |
| 0x29 | UMPatrolClear | command | o | Run 0x4B79DB | UMPatrolClear(unit): the patrol logic's point array is emptied (SetSize 0); nothing when the unit has no patrol logic. | V |
| 0x2A | UMPatrolAddPoint | command | off | Run 0x4B7A1F | UMPatrolAddPoint(unit, x, y): AiLogicPatrol::AddPoint: appends the point snapped to the centre of its 0.5 m cell ((round(2x)+0.5)*0.5) with an empty action list; nothing when the unit has no patrol logic. | V |
| 0x2B | UMPatrolAddPointLook | command | offf | Run 0x4B7A77 | UMPatrolAddPointLook(unit, x, y, delay): appends an action record {x, y, delay, 0} to the LAST patrol point (look-at point and stay time); nothing without a patrol logic or points. | V |
| 0x2C | UMStandard | command | o | Run 0x4B7E9A | UMStandard(unit): adds AiLogicAggression(0) + AiLogicSuspection + AiLogicCorpseWatcher. | V |
| 0x2D | UMAggression | command | o | Run 0x4B7F6B | UMAggression(unit): adds AiLogicAggression (kind 4) with flag 0. | V |
| 0x2E | UMRevenge | command | o | Run 0x4B7FBA | UMRevenge(unit): adds AiLogicAggression with flag 1 (the "revenge" aggression: attacks only after having been attacked / when provoked; hierarchy.md AggrMode 1). | V |
| 0x2F | UMSuspection | command | o | Run 0x4B8089 | UMSuspection(unit): adds AiLogicSuspection (kind 3: goes to the spot of a noticed stimulus). | V |
| 0x30 | UMCorpseWatcher | command | o | Run 0x4B80D1 | UMCorpseWatcher(unit): adds AiLogicCorpseWatcher (kind 5). | V |
| 0x31 | UMPlayer | command | o | Run 0x4B7AFA | UMPlayer(unit): Brain::ClearLogics, adds AiLogicPlayer (kind 6, 0x73F0F8), Brain::ClearOrder: the unit behaves like a party member. | V |
| 0xC7 | UMFear | command | of | Run 0x4B800B | UMFear(unit, flag): adds AiLogicFear (kind 7) built with Ctor(flag != 0); flag != 0 is the "always afraid" variant. | V |
| 0xC8 | UMAg | command | of | Run 0x4B7B6C | UMAg(unit, preset): removes the kinds 3 (Suspection), 4 (Aggression), 5 (CorpseWatcher), 7 (Fear), then adds: preset 0 = Fear(1) + Suspection + CorpseWatcher; 1 = Fear(0) + Suspection + CorpseWatcher; 2 = Aggression(flag 1, revenge) + Fear(0) + Suspection + CorpseWatcher; 3 = Aggression(0) + Fear(0) + Suspection + CorpseWatcher; other values: nothing is added (the old ones stay removed). The vendor docs list only presets 0..2. | V |
| 0xCA | UMFollow | command | oo | Run 0x4B7934 | UMFollow(unit, target): adds an AiLogicFollow (kind 10, class 0x73D1C4) holding the target: the unit follows permanently (AiLogicFollow::Update sets order kind 6 whenever it is idle). | V |
| 0x42 | SetSpellAggression | command | of | Run 0x4B9916 | SetSpellAggression(unit, n): the handler only calls Player_GetHeroId on the argument and discards it: no effect. | V |
| 0xA4 | SetPlayerAggression | command | ff | Run default 0x4B9BC5 (no-op) | SetPlayerAggression(a, b): not in the Run switch (default case): no effect. | V |
| 0x36 | SetPlayer | command | of | Run 0x4B70E7 | SetPlayer(unit, group): type 0x50 only: CUnit::SetAlarmMaskForce(PlayerRelations::GetForcedMask(group)) and CUnit::SetSide(group): sets the unit's side / diplomatic group (+0x250) and replaces its hostile-group mask (+0x244) by the forced mask of that side. | V |
| 0x38 | SetEnemy | command | of | Run 0x4B713A | SetEnemy(unit, group): type 0x50 only: CUnit::SetAlarmMask(mask \| 1 << group): the diplomatic group becomes hostile to the unit as if one of its members had attacked it (pure bit OR; units only). | V |
| 0x37 | SetDiplomacy | command | fff | Run 0x4B7094 | SetDiplomacy(a, b, value): PlayerRelations::Set(table g_0x7C2BC4, group a, bit b, mode, applyToUnits = 1): value > 0 mode 0 (ally), value == 0 mode 1 (neutral: both bits cleared), value < 0 mode 2 (enemy: forced bit). It is DIRECTIONAL: the relation of a toward b only; the units of the groups get their masks updated. GetDiplomacy(a, b) reads the same direction. | V |
| 0x43 | InvokeAlarm | command | fff | Run 0x4B81B2 | InvokeAlarm(n, x, y): CWorldServer::ActivateLogicSlot(slot = n, pos = (x, y, 0), force = 1). Slot 0..4 else ignored. The slot record {active byte, tick, x, y} is stored at world +0x8C + 16 n; force with n == 0 first clears all five records; then EVERY unit without a player (+0x24C == 0) switches to the logic record n of its .mob (Brain::SelectLogicSlot) and is woken up when its brain is "active". So `InvokeAlarm(0, 0, 0)` does not leave "no alarm": it resets the slots and leaves slot 0 marked active at (0, 0) (IsAlarm(0) is then 1). | V |

### 3.5 Unit and object queries

| id | name | kind | params | handler | behaviour | st |
|---|---|---|---|---|---|---|
| 0x4F | HP | function -> F | o | EvalFloat 0x4B4417 | HP(unit): current hit points = [obj+0x170]+0x28 (0.0 when the object or its specific is null). | V |
| 0x50 | MaxHP | function -> F | o | EvalFloat 0x4B444F | MaxHP(unit): [obj+0x170]+0x2C. | V |
| 0x51 | Mana | function -> F | o | EvalFloat 0x4B4487 | Mana(unit): [obj+0x170]+0x34. | V |
| 0x52 | MaxMana | function -> F | o | EvalFloat 0x4B44BF | MaxMana(unit): [obj+0x170]+0x38. | V |
| 0x53 | DistanceUnitUnit | function -> F | oo | EvalFloat 0x4B4B94 | DistanceUnitUnit(a, b): planar distance sqrt(dx^2 + dy^2) of the positions (+0x1C / +0x20), z ignored; 1e30 when either object is null. | V |
| 0x54 | DistanceUnitPoint | function -> F | off | EvalFloat 0x4B4BF8 | DistanceUnitPoint(unit, x, y): planar distance to the point; 1e30 for a null object. | V |
| 0x55 | GetX | function -> F | o | EvalFloat 0x4B44F7 | GetX(obj): object +0x1C (0.0 when null). Works on any object. | V |
| 0x56 | GetY | function -> F | o | EvalFloat 0x4B4539 | GetY(obj): object +0x20. | V |
| 0x57 | GetZ | function -> F | o | EvalFloat 0x4B4563 | GetZ(obj): ground height at (x, y) + object +0x24: CUnit::TerrainZAt for units, CWorldServer::GetHeight for other objects. | V |
| 0xBF | GetFutureX | function -> F | of | EvalFloat 0x4B469F | GetFutureX(unit, t): X of the position the object will have at world tick (now + round(t)), obtained with object vt[52] (GetPosAt, the move predictor Brain::TickFollow also uses); t in ticks (1/15 s). 0.0 for a null object. | V |
| 0xC0 | GetFutureY | function -> F | of | EvalFloat 0x4B46EC | GetFutureY(unit, t): Y of the same predicted position. | V |
| 0x63 | IsAlive | function -> F | o | EvalFloat 0x4B48C0 | IsAlive(obj): 1.0 when [obj+0x170] vt[17] returns 0 (not dead). A null object or a null specific gives 0.0, i.e. "not alive" AND "not dead". | V |
| 0x64 | IsDead | function -> F | o | EvalFloat 0x4B490B | IsDead(obj): the value of [obj+0x170] vt[17] as a float (non-zero when dead); 0.0 for null. | V |
| 0x65 | GetPlayer | function -> F | o | EvalFloat 0x4B49BF | GetPlayer(unit): the unit's side / diplomatic group (+0x250) as a float; 0.0 for non-units. (Not the owning player object, which is +0x24C.) | V |
| 0x66 | GetObjectID | function -> F | o | EvalFloat 0x4B4A8C | GetObjectID(obj): object +0x0C (the .mob id) converted to float: ids above 2^24 (character ids ~1e9) are rounded to a multiple of 64. | V |
| 0x8F | GetAIClass | function -> F | o | EvalFloat 0x4B4A04 | GetAIClass(unit): unit +0x324 (the AI class from the .mob / prototype); -1.0 for non-units. | V |
| 0x99 | IsUnitInWater | function -> F | o | EvalFloat 0x4B4752 | IsUnitInWater(obj): CAIMap::IsCellWater at the object's position: bit 7 of the terrain cell byte +6 (water flag); 0.0 for null. | V |
| 0xE3 | WasLooted | function -> F | o | EvalFloat 0x4B4663 | WasLooted(obj): bit 0x100000 of the object flags (+8) (set when the unit's loot has been taken); 1.0 for a null object. | V |
| 0xE5 | IsUnitVisible | function -> F | o | EvalFloat 0x4B52BE | IsUnitVisible(unit): 1.0 when the object pointer is in the visible-object array of player 0's connection ([player0+0x20]+0x5110, count +0x5114): visible to the host player. 0.0 for null or no player 0. | V |
| 0xE6 | IsUnitBlocked | function -> F | o | EvalFloat 0x4B545E | IsUnitBlocked(obj): object flag bit 0x800000 (BlockUnit). | V |
| 0x61 | IsEnemy | function -> F | of | EvalFloat 0x4B4951 | IsEnemy(unit, group): for units: ((1 << group) & unit+0x244 & unit+0x248) != 0 (hostile-group mask AND the unit's second mask); 0.0 for other objects. | V |
| 0x60 | GetDiplomacy | function -> F | ff | EvalFloat 0x4B485A | GetDiplomacy(a, b): PlayerRelations::GetRelation(a, b): ally 1.0, enemy -1.0, otherwise 0.0 (directional: the attitude of group a toward group b). | V |
| 0x90 | GetLeverState | function -> F | o | EvalFloat 0x4B4A49 | GetLeverState(obj): for type 0x5D objects (levers) the state index +0x238; 0.0 otherwise. | V |

### 3.6 Object lookup and groups

| id | name | kind | params | handler | behaviour | st |
|---|---|---|---|---|---|---|
| 0x4E | GetObject | function -> O | f | EvalObject 0x4B574E | GetObject(id): CWorldServer::FindObjectById(round(id)); the not-found "Object with ID %d not found" report is an empty function in this build (0x44C470), so a missing id gives a NULL reference and the commands then do nothing / queries return their defaults. Argument is a .mob id; ids of characters are too large for a float (use GetLeader / GetUnitOfPlayer / GetObjectByID). | V |
| 0x85 | GetObjectByName | function -> O | s | EvalObject 0x4B5800 | GetObjectByName(name): FindObjectById(String_HashCI(name)): the ids of named .mob objects are the case-insensitive hash of their name; null when absent. | V |
| 0xAB | GetObjectByID | function -> O | s | EvalObject 0x4B5842 | GetObjectByID(text): atoi(text) (0x6E80D8) up to 10 digits, then FindObjectById: lets scripts use full-size ids that do not fit a float. | V |
| 0xB4 | GetUnitOfPlayer | function -> O | ff | EvalObject 0x4B5794 | GetUnitOfPlayer(player, n): the unit of the n-th hero record (0x784 bytes each, unit id at +0x1C) of the player's party; null for a bad index / player. | V |
| 0xE4 | GetLeader | function -> O |  | EvalObject 0x4B589C | GetLeader(): in single player / no live lobby server: the unit of player 0's first party record (id fallback 0x3B9AF5CE); in a hosted lobby: the unit of LobbyServer::FirstReadyPlayerId (first ready player). A fresh reference is created per call. | V |
| 0x22 | AddObject | command | go | Run 0x4B6BD9 | AddObject(group, obj): ObjRefCArray_InsertSorted: a group is a set of object references sorted by pointer; inserted only when absent (pointer compare); a null object is ignored (`if (obj != 0)` in InsertSorted). | V |
| 0x23 | RemoveObject | command | go | Run 0x4B6BF0 | RemoveObject(group, obj): the first matching entry is released and removed; nothing when absent. | V |
| 0x58 | GroupSize | function -> F | g | EvalFloat 0x4B47B0 | GroupSize(group): the number of references (dead objects are dropped at the start of each Run / each Tick by PurgeDeadObjects). | V |
| 0x59 | GroupAdd | function -> G | gg | EvalGroup 0x4B5BCC | GroupAdd(g1, g2): a new temporary group = union (ObjRefCArray_Union); the temporaries live in the ctx pool (+0x1B0) until the current command ends. | V |
| 0x5A | GroupSub | function -> G | gg | EvalGroup 0x4B5BE2 | GroupSub(g1, g2): difference g1 \ g2. | V |
| 0x5B | GroupCross | function -> G | gg | EvalGroup 0x4B5BF8 | GroupCross(g1, g2): intersection. | V |
| 0x5C | GroupHas | function -> F | go | EvalFloat 0x4B47D0 | GroupHas(group, obj): 1.0 when the reference occurs (linear pointer compare). | V |
| 0x5D | UnitSee | function -> G | o | EvalGroup 0x4B5C0E | UnitSee(unit): a temporary group of the units the unit currently perceives: the "seen units" list of its AI memory (CUnitServer +0xA94 = AiMemory +0x44). Empty for non-units. | I |
| 0x5E | GroupSee | function -> G | g | EvalGroup 0x4B5C61 | GroupSee(group): the union of UnitSee over all members. | I |
| 0xA5 | PlayerSee | function -> G | f | EvalGroup 0x4B5CCA | PlayerSee(player): everything the player sees: the visible set of the player's connection ([conn]+0x50F8), its own party units and for each of them the units in its AI-memory seen list. | V |
| 0x62 | GetPlayerUnits | function -> G | f | EvalGroup 0x4B5DDF | GetPlayerUnits(n): all units (type 0x50) of the object manager whose side / diplomatic group (+0x250) equals n (NOT the owner player id). | V |

### 3.7 World: loading, spawning, areas, terrain

| id | name | kind | params | handler | behaviour | st |
|---|---|---|---|---|---|---|
| 0xA0 | AddMob | command | s | Run 0x4B9852 | AddMob(file): CWorldServer::LoadMob: loads the .mob from the MAPS folder into the running world unless already loaded (list +0x128); a failed load is a FatalError "CWorldServer::LoadMap. Can't load .mob". Its objects appear at once; its own script text is compiled only by the loader (MobFile_Load). | V |
| 0x88 | AddUnitToServer | command | sssfff | Run 0x4B8B00 | AddUnitToServer(mapName, protoName, displayName, x, y, z): CWorldServer::SpawnSingleHero(id = String_HashCI(mapName), proto = protoName, name = displayName, pos): a temporary one-hero CParty is spawned (SpawnPartyHeroes). The first string is the object name whose hash becomes the unit's id (so GetObjectByName(mapName) finds it later). The vendor name of the third argument ("NameUnitControl") is the display name. | I |
| 0x89 | RemoveUnitFromServer | command | o | Run 0x4B8B7F | RemoveUnitFromServer(obj): CWorldServer::RemoveObject(id of the object): leaves the collision map and the object hashes, vt[5] Done, units SetPlayer(null), both script VMs purge their references, every player connection untracks it, the spells aimed at it are dropped, refcount released. Same case as 0x96. | V |
| 0x96 | RemoveObjectFromServer | command | o | Run 0x4B8B7F | RemoveObjectFromServer(obj): identical code to 0x89. | V |
| 0x8A | LeaveToZone | command | fsf | Run 0x4B8BAF | LeaveToZone(player, zone, entry): CPlayerServer::ChangeMap on the player's connection (only when the player is in game): the player is taken out of the world (World::PlayerOut), its state becomes 3 (changing map), player +0xD8 = zone name and +0xDC = entry number, the party is stored (single player: PartyStore) or re-sent (SendMsg80_Party), then net opcode 8 + zone name + entry number is sent and flushed. The actual level change happens when the client/host reloads; the script goes on. In some modes (g_0x7C2CC4 = 1 or 3) a player whose own unit is missing or dead is handled first. | V |
| 0xC3 | AddRectToArea | command | fffff | Run 0x4B9B09 | AddRectToArea(id, x1, y1, x2, y2): appends a rectangle zone (type 1) to the area `id` (GSWorld::FindOrAddRecord194 creates the area when missing); IsInArea tests x1 <= x <= x2, y1 <= y <= y2, so the corners must be ordered. | V |
| 0xC4 | AddRoundToArea | command | ffff | Run 0x4B9B60 | AddRoundToArea(id, x, y, r): appends a circle zone (type 0, stored as r^2) to the area. | V |
| 0xC6 | DeleteArea | command | f | Run 0x4B9BAB | DeleteArea(id): ScriptTable_RemoveById: destroys every record with that id and shifts the rest down. | V |
| 0xC5 | IsInArea | function -> F | fff | EvalFloat 0x4B5397 | IsInArea(id, x, y): 1.0 when the point is inside any zone of the area (circle: strictly inside, d^2 < r^2; rectangle: inclusive). Creates an empty area record when the id is new. | V |
| 0xBB | IsInSquare | function -> F | ffffff | EvalFloat 0x4B51CF | IsInSquare(x, y, x1, y1, x2, y2): 1.0 when x1 <= x <= x2 and y1 <= y <= y2 (corners must be ordered). | V |
| 0xBC | UnitInSquare | function -> F | offff | EvalFloat 0x4B522D | UnitInSquare(unit, x1, y1, x2, y2): the same test on the unit's position; 0.0 for null. | V |
| 0xBD | GetZValue | function -> F | ff | EvalFloat 0x4B5332 | GetZValue(x, y): CWorldServer::GetHeight(x, y): the terrain height from the height map (0 outside the map). | V |
| 0xBE | GetBSZValue | function -> F | ff | EvalFloat 0x4B535E | GetBSZValue(x, y): calls world vt[28] = CAIMap::GetCell (a pointer to the 9-byte terrain cell is returned in EAX) but the handler stores ST(0) as the result, so the number is whatever happens to be on the FPU: unreliable. (BSZ presumably "block / passability zone".) | G |
| 0xD1 | GetMercsNumber | function -> F | f | EvalFloat 0x4B5027 | GetMercsNumber(player): CWorldServer::CountOtherLiveHeroes: the number of alive units among the player's party hero records minus one; 0 for an unknown player. | V |

### 3.8 Variables and quests

| id | name | kind | params | handler | behaviour | st |
|---|---|---|---|---|---|---|
| 0x7F | GSSetVar | command | fsf | Run 0x4B8255 | GSSetVar(player, name, value): NO live lobby server (single player / client) -> `CPlayerServer::SendMsg81_NamedValue`: the value is stored in the player's named-value map (player +0xC4, StringFloatMap_Set: setting 0 DELETES the key) and net opcode 0x81 {name, float} updates the client copy. Hosted lobby -> LobbyServer::SetVariable(name, value): ONE global table of the session (the player argument is ignored; value 0 removes it), versioned for the clients. These are the persistent quest variables (`q.<quest>.<quest>[.<n>]`). | V |
| 0x98 | GSSetVarMax | command | fsf | Run 0x4B8322 | GSSetVarMax(player, name, value): as GSSetVar but keeps the larger of old and new (SendMsg81_MaxValue / LobbyServer::MaxVariable; a zero result removes the lobby entry). | V |
| 0x82 | GSDelVar | command | fs | Run 0x4B841B | GSDelVar(player, name): lobby -> LobbyServer::RemoveVariable; else StringFloatMap_Remove on the player and net opcode 0x82 {name}. | V |
| 0x80 | GSGetVar | function -> F | fs | EvalFloat 0x4B505A | GSGetVar(player, name): lobby -> LobbyServer::GetEntryValueByName; else StringFloatMap_Get on the player (+0xC4), case-insensitive; 0.0 when missing. | V |
| 0x9F | QuestComplete | command | fs | Run 0x4B966C | QuestComplete(player, quest): single player: finds the quest in the quest table (StrHashTable_LookupId3, record +0xC = reward) and CWorldServer::GiveExperience(player, reward) + marks the party store changed. Lobby: only when the lobby's current map message equals the quest name (Lobby_NotifyMapUnloaded -> LobbyServer::OnMapUnloaded replaces it by "<name>_3") the experience goes to all players (player 0.0 = share). No journal message (that is QFinish's generated script). | I |
| 0xD5 | QStart | command | s | Run 0x4B9740 | QStart(quest): TriggerTable_Reset: empties the global objective table (g_0x7AA608) and stores the quest name (g_0x7AA61C). Must be followed by QObj* calls and QFinish in the same script. | V |
| 0xD6 | QFinish | command |  | Run 0x4B975D | QFinish(): TriggerTable_GenerateScript: pastes a WorldScript text from the table and compiles it into the running VM (Compiler_CompileText(ctx, text, 1)) which starts it: `DeclareScript Su0..SuN()`, for each objective i `Script Su<i>(if(<cond>)then(KillScript() Su<i+1>() GSSetVar(0,"q.Q.Q.<i+1>",2) [GSSetVar(0,"q.Q.Q.<i+2>",1)]))`, a last `Script SuN(if()then(KillScript() QuestComplete(0,"Q") GSSetVar(0,"q.Q.Q",2)))` and `WorldScript( Su0() )`. Objective n is 1 = received, 2 = done in `q.Q.Q.<n>`. | V |
| 0xD7 | QObjArea | command | f | Run 0x4B9769 | QObjArea(areaId): appends objective {0xD7, "", id}; condition `any(i, Heroes, IsInArea(id, GetX(i), GetY(i)))`. | V |
| 0xD8 | QObjSeeObject | command | s | Run 0x4B97A8 | QObjSeeObject(text): {0xD8, text}; condition `any(i, Heroes, IsLess(DistanceUnitUnit(i, <text>), 7))`. The string is source text pasted into the generated script. | V |
| 0xD9 | QObjSeeUnit | command | s | Run 0x4B97A8 | QObjSeeUnit(text): {0xD9}; condition `IsUnitVisible(<text>)`. | V |
| 0xDA | QObjKillUnit | command | s | Run 0x4B97A8 | QObjKillUnit(text): {0xDA}; condition `Not(IsAlive(<text>))`. | V |
| 0xDB | QObjKillGroup | command | s | Run 0x4B97A8 | QObjKillGroup(text): {0xDB}; condition `Not(any(i, <text>, IsAlive(i)))`. | V |
| 0xDC | QObjUse | command | sf | Run 0x4B97AC | QObjUse(text, state): {0xDC, text, state}; condition `IsEqual(GetLeverState(<text>), state)`. | V |
| 0xDD | QObjGetItem | command | f | Run 0x4B97D1 | QObjGetItem(scriptId): {0xDD, "", id}; condition `HaveItem(0, id)`. | V |

### 3.9 Parties, players, items and statistics

| id | name | kind | params | handler | behaviour | st |
|---|---|---|---|---|---|---|
| 0x9A | CreateParty | command | fs | Run 0x4B868B | CreateParty(player, name): PartyStore::AddParty(name) on the global party store (g_0x7AA5A0+0x24) when the player exists; the player argument is only checked, the store is global. | V |
| 0x9B | RemoveParty | command | fs | Run 0x4B86D7 | RemoveParty(player, name): PartyStore::RemoveByName(name). | V |
| 0x9C | SetCurrentParty | command | fs | Run 0x4B8723 | SetCurrentParty(player, name): stores the name in the party store's current-party string (+0xC). Names containing "::" ("Store::Hero") are resolved by ScriptParty_Resolve: the part before "::" selects the party, the rest is the hero name; the current party (g_0x7AA5D4) maps to the player's own party (+0x70). | V |
| 0x86 | AddUnitToParty | command | fss | Run 0x4B856C | AddUnitToParty(player, "Party::HeroRes", protoRes): ScriptParty_Resolve then CParty::AddHero(modelName = HeroRes, protoName = protoRes, display name ""): appends a hero record (0x784 bytes, id +0x1C = hash of the model name). | V |
| 0x87 | RemoveUnitFromParty | command | fs | Run 0x4B8604 | RemoveUnitFromParty(player, "Party::HeroRes"): CParty::RemoveHeroById(String_HashCI(HeroRes)). | V |
| 0x83 | AddUnitUnderControl | command | fo | Run 0x4B84F9 | AddUnitUnderControl(player, unit): CUnit::SetPlayer(unit, player) (sets +0x24C, registers the unit in the player's unit table). Does not touch the unit's side / diplomacy. | V |
| 0x84 | RemoveUnitFromControl | command | fo | Run 0x4B8545 | RemoveUnitFromControl(player, unit): CUnit::SetPlayer(unit, null); the player argument is ignored. | V |
| 0xD3 | RedeployParty | command | f | Run 0x4B8C0D | RedeployParty(player): World::PlayerOut(player), PartyStore StoreParty / GetParty with the player's party, a one-record fix-up of the hero position, SendMsg80_Party (the client gets the party again), marks the entry position as unset (z = -1297.0) and World::PlayerIn(player, ...): the party is respawned from the current party record. | I |
| 0xD2 | RecalcMercBriefings | command |  | Run 0x4B9030 | RecalcMercBriefings(): CWorldServer::UpdateMercenaryFlags: for every player and merc 1..10 sends the named values "b.<zone>.n1/n2/n3/n8..n11_%i" and "amerc_%i" (SendMsg81_NamedValue) from the party, the zone and the number of other live heroes. | V |
| 0xD4 | CopyItems | command | fss | Run 0x4B8922 | CopyItems(player, "A::hero", "B::hero"): the equipment of hero A (CParty::FindHeroById) is cloned onto hero B (HeroRecord::CloneItems, which first releases B's own items); both must exist. Wagon and backpacks are not touched. | V |
| 0x9D | CopyStats | command | fss | Run 0x4B8A0B | CopyStats(player, "A::hero", "B::hero"): copies 0x1C1 dwords of hero record A (+0x20 on) to B (+0x20 on) = the whole stats / skills / abilities block, and then A's display-name string (+0). Both must exist. | V |
| 0xCB | CopyLoot | command | fss | Run 0x4B8832 | CopyLoot(player, partyFrom, partyTo): the target party's item array is emptied (CParty::RemoveItem all), the source's items are cloned in (CParty::AddItem keepCount, merge) and the target's money (+0x50) is set to the source's. The source is unchanged. | V |
| 0xE0 | AddLoot | command | fss | Run 0x4B876B | AddLoot(player, partyFrom, partyTo): the source's items are cloned into the target party (merged with existing stacks) and the source's money is ADDED to the target's. Despite the vendor text ("moves") the source is not emptied. | V |
| 0xDF | FixItems | command |  | Run 0x4B9611 | FixItems(): iterates EVERY item of the item manager (all items in the world, not only the party's) and copies +0x24 over +0x20 (SSCScript::CopyCurrentPos = "current := maximum" of the first cur/max pair; hierarchy.md calls the pair "count", the item DB calls the field condition). | I |
| 0x92 | GiveQuestItem | command | fs | Run 0x4B941A | GiveQuestItem(player, name): identical code to GiveItem (same switch case): CWorldServer::CreateItemByName(name) builds ANY kind of item from its database name (quest items by their QuestItems name, others as `material.granite`), then CPlayer::AddItems(list) puts it in the player's wagon list (+0xE4). Unknown name: nothing. | V |
| 0xA6 | GiveItem | command | fs | Run 0x4B941A | GiveItem(player, name): the same code as GiveQuestItem. | V |
| 0x93 | EraseQuestItem | command | ff | Run 0x4B955F | EraseQuestItem(player, scriptId): CPlayer::RemoveSpellItemById: removes the first item of the player's list (+0xE4) of kind 0x3009 whose +0x4C equals the id (the QuestItems ScriptID), telling the client. Only the wagon list; items carried by units are not touched. | V |
| 0xA7 | GiveMoney | command | ff | Run 0x4B9399 | GiveMoney(player, n): player +0xC0 += round(n) (negative allowed, no clamp), then CPlayer::AddItems(empty list) to push the change to the client. | V |
| 0xAF | GiveUnitQuestItem | command | os | Run 0x4B94A7 | GiveUnitQuestItem(unit, name): CreateItemByName and only when the item's type id (vt[12]) is 0x14 (quest item) CUnit::AddObject4A0 appends it to the unit's own item array (+0x4A0 / count +0x4A4). Other kinds are created and silently dropped. | V |
| 0xAE | RemoveQuestItem | command | of | Run 0x4B95A4 | RemoveQuestItem(unit, scriptId): removes the first entry of the unit's +0x4A0 array whose +0x4C equals the id. | V |
| 0xB3 | GiveUnitSpell | command | os | Run 0x4B9505 | GiveUnitSpell(unit, name): CWorldServer::CreateSpellItemByName(name) (name format `spell{perk;perk}`), CUnit::AddInventoryItem puts it in the first free of the unit's 8 inventory slots (+0x460) (full: nothing), then the CUnitServer vt[29] refresh runs. | V |
| 0xCD | GiveSkill | command | osf | Run 0x4B903F | GiveSkill(unit, skill, n): the case-insensitive name selects what is raised by n (truncated to a signed char): `science` = attribute skill 0 (UnitStats_RaiseAttrSkill), `melee` 0, `archery` 1, `backstab` 2, `elemental` 3, `sense` 4, `astral` 5, `stealth` 6, `awareness` 7 (UnitStats_RaiseSkill, bytes +0x6CB[]). Max HP / mana are recomputed. Unknown names: nothing. (The docs list only the first six.) | V |
| 0xCE | GiveStrength | command | of | Run 0x4B92C1 | GiveStrength(unit, n): UnitStats_BoostMaxHP: base strength (stats +0x0C) += n and the dependent maxima are recomputed. (functions.tsv calls it "BoostMaxHP"; the script id says strength.) | I |
| 0xCF | GiveDexterity | command | of | Run 0x4B9309 | GiveDexterity(unit, n): UnitStats_BoostMaxMP: base +0x14 += n and recompute. | I |
| 0xD0 | GiveIntelligence | command | of | Run 0x4B9351 | GiveIntelligence(unit, n): UnitStats_BoostMaxStamina: base +0x1C += n and recompute. | I |
| 0x97 | GodMode | command | ff | Run 0x4B6EDA | GodMode(player, mode): the byte at player +0x108 = (char)mode. CUnitServer::TakeHit applies damage to the player's units only while this byte is 0, so any non-zero value blocks damage (the docs distinguish 1 and 2 = every hit kills; that second behaviour is not in TakeHit). | V |
| 0x7C | HaveItem | function -> F | ff | EvalFloat 0x4B4F2C | HaveItem(player, scriptId): CPlayer::HasSpellById (0x665640): 1.0 when an item of kind 0x3009 (quest item) with +0x4C == id is in the player's list or a unit of the player carries such an item in its +0x4A0 array. 0.0 for an unknown player. | V |
| 0xE1 | GetLootItemsCount | function -> F | f | EvalFloat 0x4B45E1 | GetLootItemsCount(player): player +0xEC; 0.0 for an unknown player. | V |
| 0xE2 | GetMoney | function -> F | f | EvalFloat 0x4B4622 | GetMoney(player): player +0xC0. | V |
| 0xB1 | IsPlayerInDanger | function -> F | f | EvalFloat 0x4B4F88 | IsPlayerInDanger(player): 1.0 when the connection state [player+0x20]+0x5120 == 2 (battle). | V |
| 0xB2 | IsPlayerInSafety | function -> F | f | EvalFloat 0x4B4FD7 | IsPlayerInSafety(player): 1.0 when that state == 0 (calm). | V |

### 3.10 Levers and traps

| id | name | kind | params | handler | behaviour | st |
|---|---|---|---|---|---|---|
| 0x95 | ActivateTrap | command | of | Run 0x4B6F30 | ActivateTrap(trap, flag): flag non-zero sets object flag 0x80000 (armed), zero clears it; no type check, no net-dirty call. CTrapObject::Update tests the flag every tick and triggers the trap on the units within 1.0 m while it is set. | V |
| 0xCC | EnableLever | command | of | Run 0x4B7042 | EnableLever(lever, flag): type 0x5D objects only: CTrapObject::SetDisarmed(flag != 0): non-zero CLEARS flag 0x80000, zero SETS it, plus MarkNetDirty(5). The flag is also set by CLeverObject::Use after a use, so it looks like "lever locked / used"; the vendor meaning "1 = usable" matches (misnamed function: it is applied to levers). | I |
| 0x91 | SwitchLeverState | command | of | Run 0x4B8F8B | SwitchLeverState(lever, state): type 0x5D only: CLeverObject::SwitchState(state, duration -10 = the levers database animation time (10 when not listed)): the units on the lever are lifted out of the collision map, the AI blocking cell is updated, the animation phase is started, the units are put back and, if left in a blocked cell, moved to the nearest free cell. | V |
| 0x9E | SwitchLeverStateEx | command | off | Run 0x4B8FD9 | SwitchLeverStateEx(lever, state, duration): as SwitchLeverState with the given animation duration (ticks; <= 0 = instant). | V |
| 0xAC | SetScience | command | offffff | Run 0x4B6F6E | SetScience(lever, scriptId, skill, hands, latchkey, crowbar, key): type 0x5D only: stores 3 dwords at lever +0x244: {bitmask (hands 1, lockpick 2, crowbar 4, key 8), round(scriptId), round(skill)} via CMapObject::SetPos3At244 (misnamed, it is the lever's "how can it be opened" record) and marks net-dirty 5. | V |

### 3.11 Time, weather, alarms

| id | name | kind | params | handler | behaviour | st |
|---|---|---|---|---|---|---|
| 0xB6 | RunWorldTime | command | f | Run 0x4B98E6 | RunWorldTime(h): CWorldServer::SetTimeScale(default g_0x79BE5C) then SetTime(h): the clock runs at the default speed from hour h. | V |
| 0xB7 | FixWorldTime | command | f | Run 0x4B98BE | FixWorldTime(h): SetTimeScale(0) then SetTime(h): the clock stands at hour h. | V |
| 0xB5 | GetWorldTime | function -> F |  | EvalFloat 0x4B519A | GetWorldTime(): fmod(tick * timeScale(world +0x20) + base(+0x1C), 24) hours (verified from the disassembly; the decompile is garbled). | V |
| 0xAA | IsNight | function -> F |  | EvalFloat 0x4B517C | IsNight(): the world's night value (world +0x84 float, 0..1, not a boolean: the user read 0.44 in the evening). | V |
| 0xB8 | SetWaterLevel | command | fff | Run 0x4B9810 | SetWaterLevel(basin, level, delay): table at world +0x70 of {id, steps, level, step} records: finds / adds the record `basin`, sets steps = max(1, round(delay)), step = (level - current) / steps. The table is advanced elsewhere per tick (not followed). Replication not checked. | I |
| 0x1E | SetWind | command | ffff | Run 0x4B992C | SetWind(x, y, z, force): CWorldServer::SetWind (this id's decompile still says SetLighting): stores the wind vector and strength (force clamped to 0..1) and flags every player for net update (flag 8 -> opcode 0x72). | V |
| 0x1F | SetBackGroundColor | command | fff | Run 0x4B99CF | SetBackGroundColor(r, g, b): the render globals' background colour = 0xFF000000 \| rgb (each value/255) on the LOCAL process (g_0x7AF060 + 0x14); not replicated. | V |
| 0x1 | SetSunLight | command | fffffffff | Run default 0x4B9BC5 (no-op) | SetSunLight(...): id 1 is outside the Run switch range (2..0xE8): no effect. | V |
| 0x67 | IsAlarm | function -> F | f | EvalFloat 0x4B4ABC | IsAlarm(n): the active byte of alarm slot n (world +0x8C + 16 n); n is not range checked. | V |
| 0x8B | AlarmTime | function -> F | f | EvalFloat 0x4B4AFB | AlarmTime(n): the world tick at which slot n was activated. | V |
| 0x8C | AlarmPosX | function -> F | f | EvalFloat 0x4B4B30 | AlarmPosX(n): the slot's x. | V |
| 0x8D | AlarmPosY | function -> F | f | EvalFloat 0x4B4B62 | AlarmPosY(n): the slot's y. | V |

### 3.12 Light, particle, lightning and sound effects

| id | name | kind | params | handler | behaviour | st |
|---|---|---|---|---|---|---|
| 0xB | CreatePointLight | command | ffffffff | Run 0x4B6C7A | CreatePointLight(id, x, y, z, radius, r, g, b): GSLights_Create: replaces the light record with that id (the old CEffectPointLight is removed), creates a type 0x4B effect (position, radius +0x158 - negative = dark light, colour = rgb/255) and adds it with CWorldServer::AddEffectObject (so it is replicated like any effect). | V |
| 0xC | MovePointLight | command | ffffff | Run 0x4B6C87 | MovePointLight(id, x, y, z, steps, remove): velocity = (target - position) / steps per tick; GSWorld::Tick moves the light every tick and, when the steps are used up, deletes it if `remove` is non-zero. | V |
| 0xD | DeletePointLight | command | f | Run 0x4B6C94 | DeletePointLight(id): the first record with that id: RemoveEffectObject, release. | V |
| 0xE | CreateParticleSource | command | fffffs | Run 0x4B6CA1 | CreateParticleSource(id, x, y, z, size, name): GSWorld::CreateEffect: `name` (case-insensitive) must be in the 64-entry table at 0x78B120 (AcidRay, FireArrow, FireBall, CampFire, FireBlast, LightningBlast, Fire, Smoke, VulcanSmoke, Healing, PoisonFog, Geyser, Tornado, Casting, Nuke, Mushroom, Link, BlueGas, Sphere*, ClayRing, Teleport, Modifier1..12, Antimagic, Casting*, ZoneExit, Portal, PortalStar, Cylinder1/2, FireStar, AcidStar, Sparks, VisionStar1..3, Regeneration, Silence, FeebleMind, FeetCloud1/2, CurseStars, CurseHolder, StartTrans, Transform); an unknown name does nothing. Creates a type 0x4A CEffectParticle (+0x158 = the particle type 0x2000+k, +0x15C = size) in the effect list GSWorld+0x14 under `id` (an existing id is replaced; negative id = automatic id from a down-counter). | V |
| 0x10 | MoveParticleSource | command | ffffff | Run 0x4B6CAE | MoveParticleSource(id, x, y, z, steps, remove): as MovePointLight for the particle source list (GSEffects_MoveTo). | V |
| 0x11 | DeleteParticleSource | command | f | Run 0x4B6CBB | DeleteParticleSource(id): removes the first node with that id from the effect list (ids are unique). | V |
| 0x12 | AttachParticleSource | command | ff | Run 0x4B6D1D | AttachParticleSource(id, unitId): CEffect::SetField168 (= CEffectParticle attach-unit id) on the source `id` with round(unitId) = the numeric id of the unit to follow. | V |
| 0xE7 | AttachParticles | command | fo | Run 0x4B6D92 | AttachParticles(id, obj): the same with the object's id (obj +0x0C); nothing when obj is null. | V |
| 0xBA | SetParticleSourceSize | command | ff | Run 0x4B6DFB | SetParticleSourceSize(id, size): CEffect::SetField15C (= CEffectParticle scale) of the source `id`. | V |
| 0xF | CreateLightning | command | ffffffff | Run 0x4B6E61 | CreateLightning(id, x1, y1, z1, x2, y2, z2, width): GSWorld::CreateObject4D: a type 0x4D CEffectLightning from (x1,y1,z1) to (x2,y2,z2) (CEffectLightning::SetEndpoints, width +0x164), stored in the pair list +0x4C under id (negative = auto id); an existing id is NOT replaced (a second object is added). | V |
| 0x1A | DeleteLightning | command | f | Run 0x4B6E6E | DeleteLightning(id): removes EVERY lightning node with that id from the pair list +0x50 (no break). | V |
| 0x16 | CreateFXSource | command | ffffffs | Run 0x4B81FE | CreateFXSource(id, x, y, z, volume, radius, wav): GSWorld::CreateNamedEffect (shared with 0x18) creates a type 0x48 CEffectSound at (x,y,z) with the wav name (+0x14C), volume +0x24C and radius +0x250, adds it with AddEffectObject and keeps it in the named list (GSWorld+0x34, record 0x34 bytes) under `id` (an existing id is replaced, id < 0 = automatic down-counting id); record flag +0x20 = 0. Unlike CreateFX the sound's byte +0x254 keeps the constructor value 1 (its meaning in CSoundManager::PlaySound is not resolved). GSWorld::Tick also scans this list: records with flag 0 get a countdown (+0x30), and when it drops below 1 the sound is removed and re-created around the point with the scatter / delay fields +0x24..+0x2C - fields this command does not initialise for a NEW record (Record52_CopyCtor copies an uninitialised stack temporary), so what that loop does to these records is not settled. The shipped maps use it for one-off teleport sounds and stop it with DeleteFXSource(id). | I |
| 0x17 | CreateFX | command | fffffs | Run 0x4B81F1 | CreateFX(x, y, z, volume, radius, wav): GS_CreateEffectObject48: the same sound object with +0x254 = 0, added with AddEffectObject and not kept: a one-shot sound at the point. | V |
| 0x18 | CreateRandomizedFXSource | command | fffffffffs | Run 0x4B81FE | CreateRandomizedFXSource(id, x, y, z, volume, radius, minDelay, maxDelay, scatter, wav): same function as 0x16 (not a box as the vendor text says): stores a record WITHOUT sound object under `id` with flag +0x20 = 1, scatter +0x24, minDelay +0x28, maxDelay +0x2C (rounded), countdown +0x30 = 0. The only code found that spawns from such records is the respawn loop in GSWorld::Tick (random position x/y +- scatter, random delay in [min, max] ticks), but that loop tests `flag == 0` and skips flag-1 records; so as read, this command creates a record that nothing services (the two flag values look swapped relative to the intent). Not run; treat as unreliable. | G |
| 0x19 | DeleteFXSource | command | f | Run 0x4B820B | DeleteFXSource(id): removes the first node with that id from the named effect list (its sound object is removed). | V |
| 0x1C | PlayFX | command | ffffs | Run default 0x4B9BC5 (no-op) | PlayFX(...): not in the Run switch: no effect. | V |

### 3.13 Camera, movies, music, console, messages, spells

| id | name | kind | params | handler | behaviour | st |
|---|---|---|---|---|---|---|
| 0xA1 | PlayCamera | command | s | Run 0x4B9A22 | PlayCamera(name): Camera_SetTargetTimed -> CCamera::LoadCamFile(name): loads the camera path (CAMERA folder) into the camera controller and sets camera flag 0x20 on success (IsCameraPlaying). Local to the process running the script; nothing is sent. | V |
| 0xA2 | IsCameraPlaying | function -> F |  | EvalFloat 0x4B5154 | IsCameraPlaying(): camera flags & 0x20. | V |
| 0xC1 | PlayMovie | command | s | Run 0x4B9A45 | PlayMovie(name): Movie_PlayFile("<game dir>Movies\" + name, ...) plays the Bink file synchronously (blocks the game thread until it ends or is skipped) on the local machine. | V |
| 0x1D | PlayMusic | command | fs | Run 0x4B9873 | PlayMusic(player, name): CPlayerServer::SendMsg0D_String: sends net opcode 0x0D + the name to that player's connection; the client plays STREAM\name. | V |
| 0x13 | SetCameraPosition | command | fff | Run default 0x4B9BC5 (no-op) | SetCameraPosition(x, y, z): no-op (default case; the docs and the user's test agree). | V |
| 0x14 | SetCameraOrientation | command | f | Run default 0x4B9BC5 (no-op) | SetCameraOrientation(a): no-op. | V |
| 0x6 | SetDirectionToObject | command | f | Run default 0x4B9BC5 (no-op) | SetDirectionToObject(n): no-op. | V |
| 0x21 | ShowBitmap | command | fs | Run default 0x4B9BC5 (no-op) | ShowBitmap(n, name): no-op. | V |
| 0xA3 | ShowCredits | command | s | Run default 0x4B9BC5 (no-op) | ShowCredits(name): no-op. | V |
| 0x7D | ConsoleFloat | command | f | Run 0x4B997A | ConsoleFloat(x): GSLexer_SetFlag4AndRun(cout g_0x7C55A8, x): sets the stream flag +4 = 1 and writes the value as a double (ostream::operator<<(double)), then endl; the `#` console prefix uses it. | V |
| 0x7E | ConsoleString | command | s | Run 0x4B99A2 | ConsoleString(s): `cout << s << endl` on the same stream (`$` prefix). | V |
| 0x39 | SendEvent | command | ff | Run 0x4B6EB6 | SendEvent(a, b): CPlayerManager::BroadcastMsg12(round(a), (byte)round(b)): net message 0x12 (+5 bytes) to EVERY player's connection. Neither argument is a player filter. | V |
| 0x3A | SendStringEvent | command | fs | Run 0x4B6F0F | SendStringEvent(n, text): CPlayerManager::BroadcastMsgString: for every player's connection sends the single byte 0x13 and the text, zero-terminated. The first argument is IGNORED by the sender (disassembled: SendMsgString never reads it), so the vendor "nPlayer" has no effect. The clients interpret the text ("say sNN", "tutorial", "briefing", "constr", ...). | V |
| 0xA8 | CastSpellPoint | command | sffff | Run 0x4B7569 | CastSpellPoint(spell, x1, y1, x2, y2): CSpellManager::CreateSpellByName(spell) (`spell{perk;perk}` syntax) and CWorldServer::CastSpellMulti(spell, caster = none, from (x1,y1,0), no target unit, to (x2,y2,0)); the spell object is released afterwards. Unknown spell name: nothing. | V |
| 0xA9 | CastSpellUnit | command | sffo | Run 0x4B760D | CastSpellUnit(spell, x, y, unit): CastSpellMulti with the effect start (x, y, 0) and the unit as target (its position as target point); nothing for a null unit. | V |

## 4. Index by id

Sorted by id; the section is where the behaviour is described. `Run` ids 0x2..0xE8, `EvalFloat` 0x45..0xE6 (F), `EvalObject` (O: 0x4E 0x85 0xAB 0xB4 0xE4), `EvalGroup` (G: 0x59 0x5A 0x5B 0x5D 0x5E 0x62 0xA5).

| id | name | sec | id | name | sec |
|---|---|---|---|---|---|
| 0x1 | SetSunLight | 3.11 | 0x76 | DeclareScript | 3.1 |
| 0x2 | MoveToPoint | 3.4 | 0x77 | GlobalVars | 3.1 |
| 0x3 | RotateTo | 3.4 | 0x78 | (assign) | 3.1 |
| 0x4 | MoveToObject | 3.4 | 0x79 | KillScript | 3.2 |
| 0x5 | StartAnimation | 3.4 | 0x7A | (number) | 3.1 |
| 0x6 | SetDirectionToObject | 3.13 | 0x7B | (string) | 3.1 |
| 0x7 | Sleep | 3.2 | 0x7C | HaveItem | 3.9 |
| 0x8 | (call) | 3.1 | 0x7D | ConsoleFloat | 3.13 |
| 0x9 | WaitSegment | 3.2 | 0x7E | ConsoleString | 3.13 |
| 0xA | WaitEndAnimation | 3.2 | 0x7F | GSSetVar | 3.8 |
| 0xB | CreatePointLight | 3.12 | 0x80 | GSGetVar | 3.8 |
| 0xC | MovePointLight | 3.12 | 0x82 | GSDelVar | 3.8 |
| 0xD | DeletePointLight | 3.12 | 0x83 | AddUnitUnderControl | 3.9 |
| 0xE | CreateParticleSource | 3.12 | 0x84 | RemoveUnitFromControl | 3.9 |
| 0xF | CreateLightning | 3.12 | 0x85 | GetObjectByName | 3.6 |
| 0x10 | MoveParticleSource | 3.12 | 0x86 | AddUnitToParty | 3.9 |
| 0x11 | DeleteParticleSource | 3.12 | 0x87 | RemoveUnitFromParty | 3.9 |
| 0x12 | AttachParticleSource | 3.12 | 0x88 | AddUnitToServer | 3.7 |
| 0x13 | SetCameraPosition | 3.13 | 0x89 | RemoveUnitFromServer | 3.7 |
| 0x14 | SetCameraOrientation | 3.13 | 0x8A | LeaveToZone | 3.7 |
| 0x16 | CreateFXSource | 3.12 | 0x8B | AlarmTime | 3.11 |
| 0x17 | CreateFX | 3.12 | 0x8C | AlarmPosX | 3.11 |
| 0x18 | CreateRandomizedFXSource | 3.12 | 0x8D | AlarmPosY | 3.11 |
| 0x19 | DeleteFXSource | 3.12 | 0x8E | SetCP | 3.4 |
| 0x1A | DeleteLightning | 3.12 | 0x8F | GetAIClass | 3.5 |
| 0x1B | PlayAnimation | 3.4 | 0x90 | GetLeverState | 3.5 |
| 0x1C | PlayFX | 3.12 | 0x91 | SwitchLeverState | 3.10 |
| 0x1D | PlayMusic | 3.13 | 0x92 | GiveQuestItem | 3.9 |
| 0x1E | SetWind | 3.11 | 0x93 | EraseQuestItem | 3.9 |
| 0x1F | SetBackGroundColor | 3.11 | 0x94 | Sentry | 3.4 |
| 0x21 | ShowBitmap | 3.13 | 0x95 | ActivateTrap | 3.10 |
| 0x22 | AddObject | 3.6 | 0x96 | RemoveObjectFromServer | 3.7 |
| 0x23 | RemoveObject | 3.6 | 0x97 | GodMode | 3.9 |
| 0x24 | UMClear | 3.4 | 0x98 | GSSetVarMax | 3.8 |
| 0x25 | UMSentry | 3.4 | 0x99 | IsUnitInWater | 3.5 |
| 0x26 | UMGuard | 3.4 | 0x9A | CreateParty | 3.9 |
| 0x27 | UMGuardEx | 3.4 | 0x9B | RemoveParty | 3.9 |
| 0x28 | UMPatrol | 3.4 | 0x9C | SetCurrentParty | 3.9 |
| 0x29 | UMPatrolClear | 3.4 | 0x9D | CopyStats | 3.9 |
| 0x2A | UMPatrolAddPoint | 3.4 | 0x9E | SwitchLeverStateEx | 3.10 |
| 0x2B | UMPatrolAddPointLook | 3.4 | 0x9F | QuestComplete | 3.8 |
| 0x2C | UMStandard | 3.4 | 0xA0 | AddMob | 3.7 |
| 0x2D | UMAggression | 3.4 | 0xA1 | PlayCamera | 3.13 |
| 0x2E | UMRevenge | 3.4 | 0xA2 | IsCameraPlaying | 3.13 |
| 0x2F | UMSuspection | 3.4 | 0xA3 | ShowCredits | 3.13 |
| 0x30 | UMCorpseWatcher | 3.4 | 0xA4 | SetPlayerAggression | 3.4 |
| 0x31 | UMPlayer | 3.4 | 0xA5 | PlayerSee | 3.6 |
| 0x32 | BlockUnit | 3.4 | 0xA6 | GiveItem | 3.9 |
| 0x33 | Guard | 3.4 | 0xA7 | GiveMoney | 3.9 |
| 0x34 | Idle | 3.4 | 0xA8 | CastSpellPoint | 3.13 |
| 0x35 | KillUnit | 3.4 | 0xA9 | CastSpellUnit | 3.13 |
| 0x36 | SetPlayer | 3.4 | 0xAA | IsNight | 3.11 |
| 0x37 | SetDiplomacy | 3.4 | 0xAB | GetObjectByID | 3.6 |
| 0x38 | SetEnemy | 3.4 | 0xAC | SetScience | 3.10 |
| 0x39 | SendEvent | 3.13 | 0xAD | SetCPFast | 3.4 |
| 0x3A | SendStringEvent | 3.13 | 0xAE | RemoveQuestItem | 3.9 |
| 0x3B | Follow | 3.4 | 0xAF | GiveUnitQuestItem | 3.9 |
| 0x3C | Attack | 3.4 | 0xB0 | Rest | 3.4 |
| 0x3D | Cast | 3.4 | 0xB1 | IsPlayerInDanger | 3.9 |
| 0x3E | Crawl | 3.4 | 0xB2 | IsPlayerInSafety | 3.9 |
| 0x3F | Stand | 3.4 | 0xB3 | GiveUnitSpell | 3.9 |
| 0x40 | Walk | 3.4 | 0xB4 | GetUnitOfPlayer | 3.6 |
| 0x41 | Run | 3.4 | 0xB5 | GetWorldTime | 3.11 |
| 0x42 | SetSpellAggression | 3.4 | 0xB6 | RunWorldTime | 3.11 |
| 0x43 | InvokeAlarm | 3.4 | 0xB7 | FixWorldTime | 3.11 |
| 0x44 | InflictDamage | 3.4 | 0xB8 | SetWaterLevel | 3.11 |
| 0x45 | IsEqual | 3.3 | 0xB9 | SleepUntilIdle | 3.2 |
| 0x46 | IsLess | 3.3 | 0xBA | SetParticleSourceSize | 3.12 |
| 0x47 | IsGreater | 3.3 | 0xBB | IsInSquare | 3.7 |
| 0x48 | IsEqualString | 3.3 | 0xBC | UnitInSquare | 3.7 |
| 0x49 | Add | 3.3 | 0xBD | GetZValue | 3.7 |
| 0x4A | Sub | 3.3 | 0xBE | GetBSZValue | 3.7 |
| 0x4B | Mul | 3.3 | 0xBF | GetFutureX | 3.5 |
| 0x4C | Div | 3.3 | 0xC0 | GetFutureY | 3.5 |
| 0x4D | Not | 3.3 | 0xC1 | PlayMovie | 3.13 |
| 0x4E | GetObject | 3.6 | 0xC2 | HideObject | 3.4 |
| 0x4F | HP | 3.5 | 0xC3 | AddRectToArea | 3.7 |
| 0x50 | MaxHP | 3.5 | 0xC4 | AddRoundToArea | 3.7 |
| 0x51 | Mana | 3.5 | 0xC5 | IsInArea | 3.7 |
| 0x52 | MaxMana | 3.5 | 0xC6 | DeleteArea | 3.7 |
| 0x53 | DistanceUnitUnit | 3.5 | 0xC7 | UMFear | 3.4 |
| 0x54 | DistanceUnitPoint | 3.5 | 0xC8 | UMAg | 3.4 |
| 0x55 | GetX | 3.5 | 0xC9 | Lie | 3.4 |
| 0x56 | GetY | 3.5 | 0xCA | UMFollow | 3.4 |
| 0x57 | GetZ | 3.5 | 0xCB | CopyLoot | 3.9 |
| 0x58 | GroupSize | 3.6 | 0xCC | EnableLever | 3.10 |
| 0x59 | GroupAdd | 3.6 | 0xCD | GiveSkill | 3.9 |
| 0x5A | GroupSub | 3.6 | 0xCE | GiveStrength | 3.9 |
| 0x5B | GroupCross | 3.6 | 0xCF | GiveDexterity | 3.9 |
| 0x5C | GroupHas | 3.6 | 0xD0 | GiveIntelligence | 3.9 |
| 0x5D | UnitSee | 3.6 | 0xD1 | GetMercsNumber | 3.7 |
| 0x5E | GroupSee | 3.6 | 0xD2 | RecalcMercBriefings | 3.9 |
| 0x5F | Random | 3.3 | 0xD3 | RedeployParty | 3.9 |
| 0x60 | GetDiplomacy | 3.5 | 0xD4 | CopyItems | 3.9 |
| 0x61 | IsEnemy | 3.5 | 0xD5 | QStart | 3.8 |
| 0x62 | GetPlayerUnits | 3.6 | 0xD6 | QFinish | 3.8 |
| 0x63 | IsAlive | 3.5 | 0xD7 | QObjArea | 3.8 |
| 0x64 | IsDead | 3.5 | 0xD8 | QObjSeeObject | 3.8 |
| 0x65 | GetPlayer | 3.5 | 0xD9 | QObjSeeUnit | 3.8 |
| 0x66 | GetObjectID | 3.5 | 0xDA | QObjKillUnit | 3.8 |
| 0x67 | IsAlarm | 3.11 | 0xDB | QObjKillGroup | 3.8 |
| 0x68 | Every | 3.2 | 0xDC | QObjUse | 3.8 |
| 0x69 | Any | 3.2 | 0xDD | QObjGetItem | 3.8 |
| 0x6A | Sum | 3.2 | 0xDE | SleepUntil | 3.2 |
| 0x6B | For | 3.2 | 0xDF | FixItems | 3.9 |
| 0x6C | ForIf | 3.2 | 0xE0 | AddLoot | 3.9 |
| 0x6D | `(` | 3.1 | 0xE1 | GetLootItemsCount | 3.9 |
| 0x6E | `)` | 3.1 | 0xE2 | GetMoney | 3.9 |
| 0x6F | if | 3.1 | 0xE3 | WasLooted | 3.5 |
| 0x70 | then | 3.1 | 0xE4 | GetLeader | 3.6 |
| 0x71 | `,` | 3.1 | 0xE5 | IsUnitVisible | 3.5 |
| 0x72 | `=` | 3.1 | 0xE6 | IsUnitBlocked | 3.5 |
| 0x73 | `:` | 3.1 | 0xE7 | AttachParticles | 3.12 |
| 0x74 | WorldScript | 3.1 | 0xE8 | ResetTarget | 3.4 |
| 0x75 | Script | 3.1 |  | |  |

## 5. Not resolved

Honest list of what the code read here does not settle:

- **GetBSZValue** (0xBE): the result is stored from the FPU stack although the callee returns a pointer in EAX; treat it as undefined. What "BSZ" stands for is a guess.
- **Sound-source respawn loop** (`GSWorld::Tick`, 0x4BF156): it serves records with flag `+0x20 == 0`, which are the `CreateFXSource` ones (whose timer fields are uninitialised stack data), while `CreateRandomizedFXSource` records have flag 1 and are skipped. Either the test is inverted or the randomized command is dead; neither the intent nor the runtime effect could be settled statically.
- **CreateFXSource vs CreateFX**: the only difference found is the byte `+0x254` (1 vs 0) of the `CEffectSound`; it is passed to `CSoundManager::PlaySound` as an argument whose meaning (loop? priority?) was not traced. `+0x258` ("loop") is never set by the script handlers.
- **RotateTo**: the unit order kind 2 ("direct move") with a displacement vector is queued; what `DispatchQueuedOrder` finally does with it (pure turn in place?) was not followed.
- **Cast** passes spell id 0; which spell that resolves to (the active spell slot) was not followed through `QueueCastAt`.
- **HideObject / BlockUnit / ActivateTrap**: they flip object flag bits (0x10000, 0x800000, 0x80000) without `MarkNetDirty`. Whether and how clients get these bits (probably through the generic flag byte in the net state) was not checked; the effect of 0x10000 in the renderer is not documented (briefing zones use it per the vendor text).
- **EnableLever**: the flag is set by `CLeverObject::Use` as well as by `EnableLever(0)`; the exact reading ("used up" vs "disabled") is open.
- **FixItems**: `+0x20 := +0x24` of every item; the field pair is "count" in hierarchy.md and "condition" in the vendor text.
- **GiveStrength / GiveDexterity / GiveIntelligence**: mapped to `UnitStats_BoostMaxHP / MP / Stamina` and stats `+0x0C / +0x14 / +0x1C`; the functions.tsv names describe the dependent maxima, the script ids describe the attribute.
- **SetWaterLevel**: the table at world `+0x70` is written; its per-tick consumer and the replication were not followed.
- **QuestComplete in a lobby**: the `"<name>_3"` map progression is inferred from `LobbyServer::OnMapUnloaded`.
- **UnitSee / GroupSee**: taken to be the AI memory's seen-unit list (`CUnitServer+0xA94`); the list is maintained by the perception code (not read here).
- **IsEnemy**: the second mask at unit `+0x248` is a stimulus-channel mask according to hierarchy.md; the combined test is read from the code, the gameplay meaning was not.
- **The hit record** built by `KillUnit` / `InflictDamage` (15 floats): only fields [8] (skill), [9] (damage), [11] (location), [12], [13], [14] are known from `TakeHit`; the others ([0..7], [10]) were set to the values the code uses without a meaning.
- **SendStringEvent / SendEvent receivers** (clients) were not traced; only the sender side is described.
- The use of the console VM (world +0x12A6C) by anything besides the console and `World_ScriptSpawnFlash` was not identified; the exact meaning of `CWorldServer+0x160` is only known as "set by PlayerIn".
- **Tick rate**: "1 tick = 1/15 s" is the vendor statement; `CWorldServer::Update` was not timed.

## 6. Where the code differs from the vendor reference (scripting.md)

- **AddLoot** does not move: the source wagon is left as it is (items are cloned, money added). **CopyLoot** replaces the target (items and money), also without touching the source.
- **InflictDamage** is a hit with no attacker (exactly like KillUnit, only the damage differs); the kill-experience path of `TakeHit` needs an attacker, so it grants no XP.
- **CreateRandomizedFXSource** takes (id, x, y, z, volume, radius, minDelay, maxDelay, scatter, wav): the arguments describe a respawning sound near one point, not a box (but see section 5: the code that should service it skips it).
- **SendStringEvent**'s first argument is ignored; **SendEvent** has no player filter. Both go to every connection (opcodes 0x13 / 0x12).
- **UMRevenge** is an `AiLogicAggression` with flag 1 (retaliation), not a fear logic; **UMAg** knows presets 0..3.
- There is **no `else`** and no `if` inside command lists; `Script` bodies are lists of independent `if / then` pairs that fire every tick.
- **Sleep** (or any wait) inside a `For` body ends only the iteration; **KillScript** is an error after the first `Sleep` of a body.
- **GetPlayerUnits** and **GetPlayer** work on the unit's side (diplomatic group, +0x250), not on its owner player.
- **IsNight** returns a float (the night factor), not a boolean.
- **InvokeAlarm(0, 0, 0)** clears all slots but leaves slot 0 active.
- **FixItems** repairs every item in the world's item manager, not only the party's.
- **GiveSkill** also knows `stealth` and `awareness`.
- **GSSetVar / GSGetVar / GSSetVarMax / GSDelVar** ignore the player in a hosted lobby (one session table); in all modes a value of 0 deletes the entry.
- **GetObject** of a missing id does not report anything (the error function is empty).
- **DeleteParticleSource** removes the first node with the id, **DeleteLightning** removes every node with the id; **CreateLightning** with an existing id adds a second lightning.
- **GodMode**: the byte is only tested for "non-zero" in `TakeHit`.
- The functions.tsv text of `Compiler_CompileText` has the keyword ids 0x75 / 0x76 swapped: 0x75 = `Script`, 0x76 = `DeclareScript` (this table's order is right).
