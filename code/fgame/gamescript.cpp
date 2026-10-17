/*
===========================================================================
Copyright (C) 2025 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// gamescript.cpp : Subclass of script that preprocesses labels

#include "glb_local.h"
#include "gamescript.h"
#include "scriptcompiler.h"
#include "scriptmaster.h"
#include "scriptthread.h"
#include "scriptclass.h"
#include "scriptexception.h"
#include "level.h"
#include "../sys/ps3_step.h"

static unsigned char *current_progBuffer = NULL;

str& AbstractScript::Filename(void)
{
    return Director.GetString(m_Filename);
}

const_str AbstractScript::ConstFilename(void)
{
    return m_Filename;
}

bool AbstractScript::GetSourceAt(size_t sourcePos, str *sourceLine, int& column, int& line)
{
    size_t        posLine;
    size_t        i;
    size_t        start;
    char         *p;
    char          old_token;
    sourceinfo_t *minCachedInfo = NULL;

    if (!m_SourceBuffer || sourcePos >= m_SourceLength) {
        return false;
    }

    line    = 1;
    column  = 0;
    posLine = 0;
    start   = 0;

    p = m_SourceBuffer;

    if (sourcePos > 128) {
        // Start caching above certain values

        for (i = 0; i < ARRAY_LEN(cachedInfo); i++) {
            sourceinfo_t *info = &cachedInfo[i];

            if (info->line && sourcePos > info->sourcePos
                && (!minCachedInfo || info->sourcePos > minCachedInfo->sourcePos)) {
                minCachedInfo = info;
            }
        }

        if (minCachedInfo) {
            start   = minCachedInfo->sourcePos;
            line    = minCachedInfo->line;
            column  = minCachedInfo->column;
            posLine = minCachedInfo->startLinePos;
        }
    }

    for (i = start; i < sourcePos; i++) {
        p = m_SourceBuffer + i;
        column++;

        if (*p == '\n') {
            line++;
            column = 0;
            if (i + 1 != sourcePos) {
                posLine = i + 1;
            }
        } else if (*p == '\0') {
            break;
        }
    }

    while (*p != '\0' && (*p != '\r' && *p != '\n')) {
        p++;
    }

    old_token = *p;
    *p        = '\0';

    if (sourceLine) {
        *sourceLine = (m_SourceBuffer + posLine);
    }

    *p = old_token;

    cachedInfo[cachedInfoIndex].sourcePos    = sourcePos;
    cachedInfo[cachedInfoIndex].line         = line;
    cachedInfo[cachedInfoIndex].column       = column;
    cachedInfo[cachedInfoIndex].startLinePos = posLine;
    cachedInfoIndex                          = (cachedInfoIndex + 1) % ARRAY_LEN(cachedInfo);

    return true;
}

bool AbstractScript::GetSourceAt(const unsigned char *sourcePos, str *sourceLine, int& column, int& line)
{
    const sourceinfo_t *codePos = m_ProgToSource->findKeyValue(sourcePos);

    if (!codePos) {
        return false;
    }

    return GetSourceAt(codePos->sourcePos, sourceLine, column, line);
}

void AbstractScript::PrintSourcePos(sourceinfo_t *sourcePos, bool dev)
{
    int line;
    int column;
    str sourceLine;

    if (GetSourceAt(sourcePos->sourcePos, &sourceLine, column, line)) {
        PrintSourcePos(sourceLine, column, line, dev);
    } else {
        gi.DPrintf(
            "file '%s', source pos %d line %d column %d:\n",
            Filename().c_str(),
            sourcePos->sourcePos,
            sourcePos->line,
            sourcePos->column
        );
    }
}

void AbstractScript::PrintSourcePos(size_t sourcePos, bool dev)
{
    int line;
    int column;
    str sourceLine;

    if (GetSourceAt(sourcePos, &sourceLine, column, line)) {
        PrintSourcePos(sourceLine, column, line, dev);
    } else {
        gi.DPrintf("file '%s', source pos %d:\n", Filename().c_str(), sourcePos);
    }
}

void AbstractScript::PrintSourcePos(unsigned char *m_pCodePos, bool dev)
{
    if (!m_ProgToSource) {
        return;
    }

    sourceinfo_t *codePos = m_ProgToSource->findKeyValue(m_pCodePos);

    if (!codePos) {
        return;
    }

    PrintSourcePos(codePos, dev);
}

void AbstractScript::PrintSourcePos(str sourceLine, int column, int line, bool dev)
{
    int i;
    str markerLine = "";

    for (i = 0; i < column; i++) {
        markerLine.append(sourceLine[i]);
    }

    markerLine.append("^");

    gi.DPrintf("(%s, %d):\n%s\n%s\n", Filename().c_str(), line, sourceLine.c_str(), markerLine.c_str());
}

AbstractScript::AbstractScript()
{
    m_ProgToSource  = NULL;
    m_SourceBuffer  = NULL;
    m_SourceLength  = 0;
    cachedInfoIndex = 0;
}

StateScript::StateScript()
{
    m_Parent = NULL;
}

template<>
void con_set<const_str, script_label_t>::Entry::Archive(Archiver& arc)
{
    unsigned int offset;

    Director.ArchiveString(arc, key);

    if (arc.Saving()) {
        offset = value.codepos - current_progBuffer;
        arc.ArchiveUnsigned(&offset);
    } else {
        arc.ArchiveUnsigned(&offset);
        value.codepos = current_progBuffer + offset;
        value.key     = key;
    }

    arc.ArchiveBool(&value.isprivate);
}

void StateScript::Archive(Archiver& arc)
{
    label_list.Archive(arc);
}

bool StateScript::AddLabel(const_str label, unsigned char *pos, bool private_section)
{
    if (label_list.findKeyValue(label)) {
        return false;
    }

    script_label_t& s = label_list.addKeyValue(label);

    s.codepos   = pos;
    s.key       = label;
    s.isprivate = private_section;

    if (!label_list.findKeyValue(STRING_NULL)) {
        label_list.addKeyValue(STRING_NULL) = s;
    }

    return true;
}

bool StateScript::AddLabel(str label, unsigned char *pos, bool private_section)
{
    return AddLabel(Director.AddString(label), pos, private_section);
}

unsigned char *StateScript::FindLabel(const_str label)
{
    script_label_t *s;
    ScriptClass    *scriptClass;
    GameScript     *script;

    s = label_list.findKeyValue(label);

    if (s) {
        // check if the label is a private function
        if (s->isprivate) {
            scriptClass = Director.CurrentScriptClass();

            if (scriptClass) {
                script = scriptClass->GetScript();

                // now check if the label's statescript matches this statescript
                if ((&script->m_State) != this) {
                    ScriptError("Cannot call a private function.");
                    return NULL;
                }
            }
        }

        return s->codepos;
    } else {
        return NULL;
    }
}

unsigned char *StateScript::FindLabel(str label)
{
    return FindLabel(Director.AddString(label));
}

const_str StateScript::NearestLabel(unsigned char *pos)
{
    unsigned int                                    offset  = pos - m_Parent->m_ProgBuffer;
    unsigned int                                    bestOfs = 0;
    const_str                                       label   = STRING_NULL;
    con_set_enum<const_str, script_label_t>         en      = label_list;
    con_set_enum<const_str, script_label_t>::Entry *entry;

    for (entry = en.NextElement(); entry; entry = en.NextElement()) {
        const script_label_t& l = entry->value;

        if ((l.codepos - m_Parent->m_ProgBuffer) >= bestOfs) {
            bestOfs = l.codepos - m_Parent->m_ProgBuffer;

            if (bestOfs > offset) {
                break;
            }

            label = l.key;
        }
    }

    return label;
}

GameScript::GameScript()
{
    m_Filename     = STRING_NULL;
    successCompile = false;

    m_ProgBuffer   = NULL;
    m_ProgLength   = 0;
    m_bPrecompiled = false;

    requiredStackSize = 0;

    m_State.m_Parent = this;
}

GameScript::GameScript(const char *filename)
{
    m_Filename     = Director.AddString(filename);
    successCompile = false;

    m_ProgBuffer   = NULL;
    m_ProgLength   = 0;
    m_ProgToSource = NULL;
    m_bPrecompiled = false;

    m_SourceBuffer = NULL;
    m_SourceLength = 0;

    requiredStackSize = 0;

    m_State.m_Parent = this;
}

GameScript::~GameScript()
{
    Close();
}

struct pfixup_t {
    bool          isString;
    unsigned char *ptr;
};

static unsigned int ReadPackedUnsigned(const unsigned char *source)
{
    unsigned int value = 0;
#if defined(__PS3__)
    volatile unsigned char *dst = reinterpret_cast<volatile unsigned char *>(&value);
    const volatile unsigned char *src = reinterpret_cast<const volatile unsigned char *>(source);
    for (size_t i = 0; i < sizeof(value); ++i) dst[i] = src[i];
#else
    memcpy(&value, source, sizeof(value));
#endif
    return value;
}

static void WritePackedUnsigned(unsigned char *destination, unsigned int value)
{
#if defined(__PS3__)
    volatile unsigned char *dst = reinterpret_cast<volatile unsigned char *>(destination);
    const volatile unsigned char *src = reinterpret_cast<const volatile unsigned char *>(&value);
    for (size_t i = 0; i < sizeof(value); ++i) dst[i] = src[i];
#else
    memcpy(destination, &value, sizeof(value));
#endif
}

static Container<const_str>  archivedEvents;
static Container<const_str>  archivedStrings;
static Container<pfixup_t *> archivedPointerFixup;

void ArchiveOpcode(Archiver& arc, unsigned char *code)
{
    unsigned int index;

    arc.ArchiveByte(code);

    switch (*code) {
    case OP_STORE_NIL:
    case OP_STORE_NULL:
    case OP_DONE:
        break;

    case OP_EXEC_CMD_COUNT1:
    case OP_EXEC_CMD_METHOD_COUNT1:
    case OP_EXEC_METHOD_COUNT1:
        arc.ArchiveByte(code + 1);
        goto __exec;

    case OP_EXEC_CMD0:
    case OP_EXEC_CMD1:
    case OP_EXEC_CMD2:
    case OP_EXEC_CMD3:
    case OP_EXEC_CMD4:
    case OP_EXEC_CMD5:
    case OP_EXEC_CMD_METHOD0:
    case OP_EXEC_CMD_METHOD1:
    case OP_EXEC_CMD_METHOD2:
    case OP_EXEC_CMD_METHOD3:
    case OP_EXEC_CMD_METHOD4:
    case OP_EXEC_CMD_METHOD5:
    case OP_EXEC_METHOD0:
    case OP_EXEC_METHOD1:
    case OP_EXEC_METHOD2:
    case OP_EXEC_METHOD3:
    case OP_EXEC_METHOD4:
    case OP_EXEC_METHOD5:
        code--;
    __exec:
        if (!arc.Loading()) {
            index = archivedEvents.AddUniqueObject((const_str)ReadPackedUnsigned(code + 2));
        }

        arc.ArchiveUnsigned(&index);

        if (arc.Loading()) {
            pfixup_t *p = new pfixup_t;

            p->isString = false;
            p->ptr      = code + 2;

            WritePackedUnsigned(code + 2, index);
            archivedPointerFixup.AddObject(p);
        }
        break;

    case OP_LOAD_FIELD_VAR:
    case OP_LOAD_GAME_VAR:
    case OP_LOAD_GROUP_VAR:
    case OP_LOAD_LEVEL_VAR:
    case OP_LOAD_LOCAL_VAR:
    case OP_LOAD_OWNER_VAR:
    case OP_LOAD_PARM_VAR:
    case OP_LOAD_SELF_VAR:
    case OP_LOAD_STORE_GAME_VAR:
    case OP_LOAD_STORE_GROUP_VAR:
    case OP_LOAD_STORE_LEVEL_VAR:
    case OP_LOAD_STORE_LOCAL_VAR:
    case OP_LOAD_STORE_OWNER_VAR:
    case OP_LOAD_STORE_PARM_VAR:
    case OP_LOAD_STORE_SELF_VAR:
    case OP_STORE_FIELD:
    case OP_STORE_FIELD_REF:
    case OP_STORE_GAME_VAR:
    case OP_STORE_GROUP_VAR:
    case OP_STORE_LEVEL_VAR:
    case OP_STORE_LOCAL_VAR:
    case OP_STORE_OWNER_VAR:
    case OP_STORE_PARM_VAR:
    case OP_STORE_SELF_VAR:
    case OP_STORE_STRING:
        if (!arc.Loading()) {
            index = archivedStrings.AddUniqueObject((const_str)ReadPackedUnsigned(code + 1));
        }

        arc.ArchiveUnsigned(&index);

        if (arc.Loading()) {
            pfixup_t *p = new pfixup_t;

            p->isString = true;
            p->ptr      = code + 1;

            WritePackedUnsigned(code + 1, index);
            archivedPointerFixup.AddObject(p);
        }
        break;

    default:
        if (OpcodeLength(*code) > 1) {
            arc.ArchiveRaw(code + 1, OpcodeLength(*code) - 1);
        }
    }
}

template<>
void con_set<unsigned char *, sourceinfo_t>::Entry::Archive(Archiver& arc)
{
    unsigned int offset;

    if (arc.Loading()) {
        arc.ArchiveUnsigned(&offset);
        key = current_progBuffer + offset;
    } else {
        offset = key - current_progBuffer;
        arc.ArchiveUnsigned(&offset);
    }

    arc.ArchiveUnsigned(&value.sourcePos);
    arc.ArchiveInteger(&value.column);
    arc.ArchiveInteger(&value.line);
}

void GameScript::Archive(Archiver& arc)
{
    long programLength = arc.Saving() ? (long)m_ProgLength : 0;
    int i;
    bool sawDone = false;

    if (arc.Saving() && (!m_ProgBuffer || !m_ProgLength)) {
        arc.FileError("Cannot archive an empty compiled script");
        return;
    }

    arc.ArchiveSize(&programLength);
    if (!arc.NoErrors()) {
        return;
    }

    if (arc.Loading()) {
        /* Reject invalid/corrupt lengths before allocating memory on the PS3. */
        if (programLength < 1 || programLength > 16 * 1024 * 1024) {
            arc.FileError("Compiled script has an invalid bytecode length");
            return;
        }
        m_ProgLength = (size_t)programLength;
        m_ProgBuffer = (unsigned char *)gi.Malloc(m_ProgLength);
        if (!m_ProgBuffer) {
            arc.FileError("Not enough memory for compiled script bytecode");
            return;
        }
    } else if ((unsigned long)programLength != (unsigned long)m_ProgLength) {
        arc.FileError("Compiled script bytecode length overflow");
        return;
    }

    current_progBuffer = m_ProgBuffer;

    /* Bytecode has packed operands; ArchiveOpcode handles pointer-bearing
     * operands separately so cache files contain no process addresses. Check
     * each opcode length before ArchiveOpcode touches any operand bytes. */
    size_t offset = 0;
    while (offset < m_ProgLength && arc.NoErrors()) {
        const unsigned char opcode = m_ProgBuffer[offset];
        if (opcode == OP_DONE) {
            ArchiveOpcode(arc, m_ProgBuffer + offset);
            if (arc.NoErrors()) {
                ++offset;
                sawDone = true;
            }
            break;
        }

        const size_t opcodeLength = (size_t)OpcodeLength(opcode);
        if (opcodeLength < 1 || opcodeLength > m_ProgLength - offset) {
            arc.FileError("Compiled script contains an invalid opcode length");
            break;
        }

        ArchiveOpcode(arc, m_ProgBuffer + offset);
        if (!arc.NoErrors()) {
            break;
        }
        offset += opcodeLength;
    }

    if (!arc.NoErrors() || !sawDone) {
        if (arc.NoErrors()) {
            arc.FileError("Compiled script is missing OP_DONE");
        }
        archivedStrings.FreeObjectList();
        archivedEvents.FreeObjectList();
        archivedPointerFixup.FreeObjectList();
        current_progBuffer = NULL;
        return;
    }

    /* Convert string IDs and event IDs back to process-local IDs on load. */
    if (arc.Saving()) {
        i = archivedStrings.NumObjects();
        arc.ArchiveInteger(&i);
        for (int j = i; j > 0; --j) {
            const_str value = archivedStrings.ObjectAt(j);
            Director.ArchiveString(arc, value);
        }

        i = archivedEvents.NumObjects();
        arc.ArchiveInteger(&i);
        for (int j = i; j > 0; --j) {
            command_t *eventInfo = Event::GetEventInfo(archivedEvents.ObjectAt(j));
            if (!eventInfo) {
                arc.FileError("Compiled script references an unknown event");
                break;
            }
            str command = eventInfo->command;
            int flags = eventInfo->flags;
            byte type = eventInfo->type;
            arc.ArchiveString(&command);
            arc.ArchiveInteger(&flags);
            arc.ArchiveByte(&type);
        }
    } else {
        i = 0;
        arc.ArchiveInteger(&i);
        if (i < 0 || i > 65536) {
            arc.FileError("Compiled script string table is invalid");
        } else {
            archivedStrings.Resize(i + 1);
            for (int j = i; j > 0 && arc.NoErrors(); --j) {
                const_str value = STRING_NULL;
                Director.ArchiveString(arc, value);
                archivedStrings.AddObjectAt(j, value);
            }
        }

        i = 0;
        if (arc.NoErrors()) {
            arc.ArchiveInteger(&i);
        }
        if (i < 0 || i > 65536) {
            arc.FileError("Compiled script event table is invalid");
        } else {
            archivedEvents.Resize(i + 1);
            for (int j = i; j > 0 && arc.NoErrors(); --j) {
                str command;
                int flags = 0;
                byte type = EV_NORMAL;
                arc.ArchiveString(&command);
                arc.ArchiveInteger(&flags);
                arc.ArchiveByte(&type);
                if (arc.NoErrors()) {
                    const int eventNum = Event::GetEventWithFlags(command, flags, type);
                    if (!eventNum) {
                        arc.FileError("Compiled script cache references an unavailable event");
                        break;
                    }
                    archivedEvents.AddObjectAt(j, (const_str)eventNum);
                }
            }
        }

        if (arc.NoErrors()) {
            for (i = archivedPointerFixup.NumObjects(); i > 0; --i) {
                pfixup_t *fixup = archivedPointerFixup.ObjectAt(i);
                const unsigned int index = ReadPackedUnsigned(fixup->ptr);
                const unsigned int value = fixup->isString
                    ? (unsigned int)archivedStrings.ObjectAt((int)index)
                    : (unsigned int)archivedEvents.ObjectAt((int)index);
                WritePackedUnsigned(fixup->ptr, value);
                delete fixup;
            }
            archivedPointerFixup.ClearObjectList();
        }
    }

    /* Preserve source locations as bytecode offsets. Without this map, a
     * cache hit would lose the script/source correlation used by diagnostics. */
    if (arc.NoErrors()) {
        int sourceMapCount = (arc.Saving() && m_ProgToSource)
            ? (int)m_ProgToSource->size() : 0;
        arc.ArchiveInteger(&sourceMapCount);
        if (sourceMapCount < 0 || sourceMapCount > 1048576) {
            arc.FileError("Compiled script source map is invalid");
        } else if (arc.Loading()) {
            if (m_ProgToSource) {
                delete m_ProgToSource;
                m_ProgToSource = NULL;
            }
            m_ProgToSource = new con_set<const unsigned char *, sourceinfo_t>;
            m_ProgToSource->resize(sourceMapCount);
            for (int j = 0; j < sourceMapCount && arc.NoErrors(); ++j) {
                unsigned int codeOffset = 0;
                sourceinfo_t info;
                arc.ArchiveUnsigned(&codeOffset);
                arc.ArchiveUnsigned(&info.sourcePos);
                arc.ArchiveUnsigned(&info.startLinePos);
                arc.ArchiveInteger(&info.column);
                arc.ArchiveInteger(&info.line);
                if (!arc.NoErrors()) {
                    break;
                }
                if (codeOffset >= m_ProgLength) {
                    arc.FileError("Compiled script source-map offset is invalid");
                    break;
                }
                m_ProgToSource->addKeyValue(m_ProgBuffer + codeOffset) = info;
            }
        } else if (m_ProgToSource) {
            con_set_enum<const unsigned char *, sourceinfo_t> entries(*m_ProgToSource);
            typename con_set<const unsigned char *, sourceinfo_t>::Entry *entry;
            while ((entry = entries.NextElement()) != NULL && arc.NoErrors()) {
                const unsigned char *key = entry->GetKey();
                if (key < m_ProgBuffer || key >= m_ProgBuffer + m_ProgLength) {
                    arc.FileError("Compiled script has an invalid source-map pointer");
                    break;
                }
                unsigned int codeOffset = (unsigned int)(key - m_ProgBuffer);
                sourceinfo_t info = entry->value;
                arc.ArchiveUnsigned(&codeOffset);
                arc.ArchiveUnsigned(&info.sourcePos);
                arc.ArchiveUnsigned(&info.startLinePos);
                arc.ArchiveInteger(&info.column);
                arc.ArchiveInteger(&info.line);
            }
        }
    }

    if (arc.NoErrors()) {
        m_State.m_Parent = this;
        m_State.Archive(arc);

        int switchStateCount = arc.Saving() ? m_StateScripts.NumObjects() : 0;
        arc.ArchiveInteger(&switchStateCount);
        if (switchStateCount < 0 || switchStateCount > 4096) {
            arc.FileError("Compiled script state table is invalid");
        } else if (arc.Loading()) {
            for (i = 0; i < switchStateCount && arc.NoErrors(); ++i) {
                StateScript *state = new StateScript;
                state->m_Parent = this;
                state->Archive(arc);
                m_StateScripts.AddObject(state);
            }
        } else {
            for (i = 1; i <= switchStateCount && arc.NoErrors(); ++i) {
                m_StateScripts.ObjectAt(i)->Archive(arc);
            }
        }

        int catchCount = arc.Saving() ? m_CatchBlocks.NumObjects() : 0;
        arc.ArchiveInteger(&catchCount);
        if (catchCount < 0 || catchCount > 4096) {
            arc.FileError("Compiled script catch table is invalid");
        } else if (arc.Loading()) {
            for (i = 0; i < catchCount && arc.NoErrors(); ++i) {
                unsigned int startOffset = 0;
                unsigned int endOffset = 0;
                arc.ArchiveUnsigned(&startOffset);
                arc.ArchiveUnsigned(&endOffset);
                if (!arc.NoErrors()) {
                    break;
                }
                if (startOffset >= m_ProgLength || endOffset >= m_ProgLength || startOffset >= endOffset) {
                    arc.FileError("Compiled script catch offset is invalid");
                    break;
                }
                CatchBlock *catchBlock = new CatchBlock;
                catchBlock->m_TryStartCodePos = m_ProgBuffer + startOffset;
                catchBlock->m_TryEndCodePos = m_ProgBuffer + endOffset;
                catchBlock->m_StateScript.m_Parent = this;
                catchBlock->m_StateScript.Archive(arc);
                m_CatchBlocks.AddObject(catchBlock);
            }
        } else {
            for (i = 1; i <= catchCount && arc.NoErrors(); ++i) {
                CatchBlock *catchBlock = m_CatchBlocks.ObjectAt(i);
                unsigned int startOffset = (unsigned int)(catchBlock->m_TryStartCodePos - m_ProgBuffer);
                unsigned int endOffset = (unsigned int)(catchBlock->m_TryEndCodePos - m_ProgBuffer);
                arc.ArchiveUnsigned(&startOffset);
                arc.ArchiveUnsigned(&endOffset);
                catchBlock->m_StateScript.Archive(arc);
            }
        }
    }

    if (arc.NoErrors()) {
        arc.ArchiveUnsigned(&requiredStackSize);
        arc.ArchiveBool(&m_bPrecompiled);
    }

    archivedStrings.FreeObjectList();
    archivedEvents.FreeObjectList();
    archivedPointerFixup.FreeObjectList();
    current_progBuffer = NULL;

    if (arc.Loading() && arc.NoErrors()) {
        successCompile = true;
    }
}

void GameScript::Archive(Archiver& arc, GameScript *& scr)
{
    str filename;

    if (arc.Saving()) {
        if (scr) {
            filename = scr->Filename();
        } else {
            filename = "";
        }

        arc.ArchiveString(&filename);
    } else {
        arc.ArchiveString(&filename);

        if (filename != "") {
            scr = Director.GetScript(filename);
        } else {
            scr = NULL;
        }
    }
}

void GameScript::ArchiveCodePos(Archiver& arc, unsigned char **codePos)
{
    int pos = 0;
    str filename;

    if (arc.Saving()) {
        GetCodePos(*codePos, filename, pos);
    }

    arc.ArchiveInteger(&pos);
    arc.ArchiveString(&filename);

    if (arc.Loading()) {
        SetCodePos(*codePos, filename, pos);
    }
}

void GameScript::Close(void)
{
    // Free up catch blocks
    for (int i = m_CatchBlocks.NumObjects(); i > 0; i--) {
        delete m_CatchBlocks.ObjectAt(i);
    }

    // Added in OPM
    //  Free up allocated state scripts
    for (int i = m_StateScripts.NumObjects(); i > 0; i--) {
        delete m_StateScripts.ObjectAt(i);
    }

    m_CatchBlocks.FreeObjectList();

    if (m_ProgToSource) {
        delete m_ProgToSource;
        m_ProgToSource = NULL;
    }

    if (m_ProgBuffer) {
        gi.Free(m_ProgBuffer);
        m_ProgBuffer = NULL;
    }

    if (m_SourceBuffer) {
        gi.Free(m_SourceBuffer);
        m_SourceBuffer = NULL;
    }

    m_ProgLength   = 0;
    m_SourceLength = 0;
    m_bPrecompiled = false;
}

void GameScript::Load(const void *sourceBuffer, size_t sourceLength)
{
#if defined(__PS3__)
    const qboolean ps3TraceAttackScript = !Q_stricmp(Filename().c_str(), "anim/attack.scr");
    if (ps3TraceAttackScript) PS3_BootTraceFastf("ATTACK_SCRIPT GameScript-Load enter file=%s bytes=%u", Filename().c_str(), (unsigned int)sourceLength);
#endif
    size_t nodeLength;
    char  *m_PreprocessedBuffer;

    m_SourceBuffer = (char *)gi.Malloc(sourceLength + 2);
    m_SourceLength = sourceLength;

    // Original mohaa doesn't reallocate the input string to append a newline
    // This is a temporary workaround to handle the absolute disaster of newlines
    // Both the lexer and grammar are extremely abhorrent at handling newlines followed by an EOF
    m_SourceBuffer[sourceLength]     = '\n';
    m_SourceBuffer[sourceLength + 1] = 0;

    memcpy(m_SourceBuffer, sourceBuffer, sourceLength);
#if defined(__PS3__)
    if (ps3TraceAttackScript) PS3_BootTraceFastf("ATTACK_SCRIPT source-copy file=%s bytes=%u", Filename().c_str(), (unsigned int)sourceLength);
#endif

    Compiler.Reset();

#if defined(__PS3__)
    if (ps3TraceAttackScript) PS3_BootTraceFastf("ATTACK_SCRIPT preprocess-enter file=%s", Filename().c_str());
#endif
    m_PreprocessedBuffer = Compiler.Preprocess(m_SourceBuffer);
#if defined(__PS3__)
    if (ps3TraceAttackScript) PS3_BootTraceFastf("ATTACK_SCRIPT preprocess-exit file=%s buffer=%p", Filename().c_str(), m_PreprocessedBuffer);
#endif
#if defined(__PS3__)
    if (ps3TraceAttackScript) PS3_BootTraceFastf("ATTACK_SCRIPT parse-enter file=%s", Filename().c_str());
#endif
    if (!Compiler.Parse(this, m_PreprocessedBuffer, "script", nodeLength)) {
#if defined(__PS3__)
        if (ps3TraceAttackScript) PS3_BootTraceFastf("ATTACK_SCRIPT parse-failed file=%s", Filename().c_str());
#endif
        gi.DPrintf2("^~^~^ Script file compile error:  Couldn't parse '%s'\n", Filename().c_str());
        return Close();
    }
#if defined(__PS3__)
    if (ps3TraceAttackScript) PS3_BootTraceFastf("ATTACK_SCRIPT parse-exit file=%s nodeBytes=%u", Filename().c_str(), (unsigned int)nodeLength);
#endif

    if (nodeLength == 0) {
        // No code, assume success
        requiredStackSize = 0;
        successCompile    = true;
        return;
    }

#if defined(__PS3__)
    if (ps3TraceAttackScript) PS3_BootTraceFastf("ATTACK_SCRIPT preclean-enter file=%s", Filename().c_str());
#endif
    Compiler.Preclean(m_PreprocessedBuffer);
#if defined(__PS3__)
    if (ps3TraceAttackScript) PS3_BootTraceFastf("ATTACK_SCRIPT preclean-exit file=%s", Filename().c_str());
#endif

    m_ProgBuffer = (unsigned char *)gi.Malloc(nodeLength);
#if defined(__PS3__)
    if (ps3TraceAttackScript) PS3_BootTraceFastf("ATTACK_SCRIPT compile-enter file=%s nodeBytes=%u", Filename().c_str(), (unsigned int)nodeLength);
#endif

    if (!Compiler.Compile(this, m_ProgBuffer, m_ProgLength)) {
#if defined(__PS3__)
        if (ps3TraceAttackScript) PS3_BootTraceFastf("ATTACK_SCRIPT compile-failed file=%s", Filename().c_str());
#endif
        gi.DPrintf2("^~^~^ Script file compile error:  Couldn't compile '%s'\n", Filename().c_str());
        return Close();
    }

#if defined(__PS3__)
    if (ps3TraceAttackScript) PS3_BootTraceFastf("ATTACK_SCRIPT compile-exit file=%s progBytes=%u", Filename().c_str(), (unsigned int)m_ProgLength);
#endif
    requiredStackSize = Compiler.m_iInternalMaxVarStackOffset + 9 * Compiler.m_iMaxExternalVarStackOffset + 1;

    successCompile = true;
#if defined(__PS3__)
    if (ps3TraceAttackScript) PS3_BootTraceFastf("ATTACK_SCRIPT GameScript-Load exit file=%s success=%d", Filename().c_str(), successCompile);
#endif
}

bool GameScript::GetCodePos(unsigned char *codePos, str& filename, int& pos)
{
    pos = codePos - m_ProgBuffer;

    if (pos >= 0 && pos < m_ProgLength) {
        filename = Filename();
        return true;
    } else {
        return false;
    }
}

bool GameScript::SetCodePos(unsigned char *& codePos, str& filename, int pos)
{
    if (Filename() == filename) {
        codePos = m_ProgBuffer + pos;
        return true;
    } else {
        return false;
    }
}

unsigned int GameScript::GetRequiredStackSize(void)
{
    return requiredStackSize;
}

qboolean GameScript::labelExists(const char *name)
{
    str labelname;

    // if we got passed a NULL than that means just run the script so of course it exists
    if (!name) {
        return true;
    }

    if (m_State.FindLabel(name)) {
        return true;
    }

    return false;
}

StateScript *GameScript::CreateCatchStateScript(unsigned char *try_begin_code_pos, unsigned char *try_end_code_pos)
{
    CatchBlock *catchBlock = new CatchBlock;

    catchBlock->m_TryStartCodePos = try_begin_code_pos;
    catchBlock->m_TryEndCodePos   = try_end_code_pos;

    m_CatchBlocks.AddObject(catchBlock);

    return &catchBlock->m_StateScript;
}

StateScript *GameScript::CreateSwitchStateScript(void)
{
    StateScript *stateScript = new StateScript;

    m_StateScripts.AddObject(stateScript);

    return stateScript;
}

StateScript *GameScript::GetCatchStateScript(unsigned char *in, unsigned char *& out)
{
    CatchBlock *catchBlock;
    CatchBlock *bestCatchBlock = NULL;

    for (int i = m_CatchBlocks.NumObjects(); i > 0; i--) {
        catchBlock = m_CatchBlocks.ObjectAt(i);

        if (in >= catchBlock->m_TryStartCodePos && in < catchBlock->m_TryEndCodePos) {
            if (!bestCatchBlock || catchBlock->m_TryEndCodePos < bestCatchBlock->m_TryEndCodePos) {
                bestCatchBlock = catchBlock;
            }
        }
    }

    if (bestCatchBlock) {
        out = bestCatchBlock->m_TryEndCodePos;

        return &bestCatchBlock->m_StateScript;
    } else {
        return NULL;
    }
}

bool GameScript::ScriptCheck(void)
{
    if (g_scriptcheck->integer == 1) {
        return true;
    }

    if (g_scriptcheck->integer == 2 || g_scriptcheck->integer == 3) {
        if (strstr(Filename().c_str(), "anim/") != Filename().c_str()) {
            return true;
        }
    }
    if (g_scriptcheck->integer == 3) {
        if (strstr(Filename().c_str(), "global/") != Filename().c_str()) {
            return true;
        }
    }
    return false;
}

ScriptThreadLabel::ScriptThreadLabel()
{
    m_Script = NULL;
    m_Label  = STRING_EMPTY;
}

ScriptThread *ScriptThreadLabel::Create(Listener *listener) const
{
    ScriptClass  *scriptClass;
    ScriptThread *thread;

    if (!m_Script) {
        return NULL;
    }

    scriptClass = NULL;
    thread      = NULL;

    try {
        scriptClass = new ScriptClass(m_Script, listener);
        thread      = new ScriptThread(scriptClass, m_Script->m_State.FindLabel(m_Label));
    } catch (...) {
        if (scriptClass) {
            delete scriptClass;
        }

        if (thread) {
            delete thread;
        }

        throw;
    }

    return thread;
}

void ScriptThreadLabel::Execute(Listener *listener) const
{
    if (!m_Script) {
        return;
    }

    ScriptThread *thread = Create(listener);

    if (thread) {
        thread->Execute();
    }
}

void ScriptThreadLabel::Execute(Listener *listener, Event& ev) const
{
    if (!m_Script) {
        return;
    }

    ScriptThread *thread = Create(listener);

    if (thread) {
        thread->Execute(ev);
    }
}

void ScriptThreadLabel::Execute(Listener *listener, Event *ev) const
{
    Execute(listener, *ev);
}

void ScriptThreadLabel::Execute(
    Listener *pSelf, const SafePtr<Listener>& listener, const SafePtr<Listener>& param
) const
{
    if (!m_Script) {
        return;
    }

    ScriptVariable params[2];

    params[0].setListenerValue(listener);
    params[1].setListenerValue(param);

    ScriptClass  *scriptClass = new ScriptClass(m_Script, pSelf);
    ScriptThread *thread      = Director.CreateScriptThread(scriptClass, m_Label);

    if (thread) {
        thread->Execute(params, 2);
    }
}

void ScriptThreadLabel::Clear()
{
    m_Script = NULL;
    m_Label  = STRING_EMPTY;
}

void ScriptThreadLabel::Set(const char *label)
{
    str   script;
    char  buffer[1023];
    char *p          = buffer;
    int   i          = 0;
    bool  foundLabel = false;

    if (!label || !*label) {
        m_Script = NULL;
        m_Label  = STRING_EMPTY;
        return;
    }

    Q_strncpyz(buffer, label, sizeof(buffer));

    while (true) {
        if (p[0] == ':' && p[1] == ':') {
            *p = '\0';

            script     = buffer;
            m_Label    = Director.AddString(&p[2]);
            foundLabel = true;

            break;
        }

        p++;
        i++;

        if (*p == '\0') {
            break;
        }

        if (i >= 1023) {
            //we didn't find label but this is how it works.
            //the whole string is the label
            m_Label    = Director.AddString(buffer);
            script     = buffer;
            foundLabel = true;
            break;
        }
    }

    if (!foundLabel) {
        script  = level.m_mapscript;
        m_Label = Director.AddString(buffer);
    }

    m_Script = Director.GetGameScript(script);

    if (!m_Script->m_State.FindLabel(m_Label)) {
        str l = Director.GetString(m_Label);

        m_Script = NULL;
        m_Label  = STRING_EMPTY;

        ScriptError("^~^~^ Could not find label '%s' in '%s'", l.c_str(), script.c_str());
    }
}

void ScriptThreadLabel::Set(const_str label)
{
    return Set(Director.GetString(label));
}

void ScriptThreadLabel::SetScript(const ScriptVariable& label)
{
    switch (label.GetType()) {
    case VARIABLE_STRING:
        m_Script = Director.GetGameScript(label.stringValue());
        m_Label  = STRING_EMPTY;
        break;
    case VARIABLE_CONSTSTRING:
        m_Script = Director.GetGameScript(label.constStringValue());
        m_Label  = STRING_EMPTY;
        break;
    case VARIABLE_NONE:
        m_Script = NULL;
        m_Label  = STRING_EMPTY;
        break;
    case VARIABLE_CONSTARRAY:
        if (label.arraysize() > 1) {
            ScriptVariable *script    = label[1];
            ScriptVariable *labelname = label[2];

            if (script->GetType() == VARIABLE_CONSTSTRING) {
                m_Script = Director.GetGameScript(script->constStringValue());
            } else {
                m_Script = Director.GetGameScript(script->stringValue());
            }

            m_Label = labelname->constStringValue();
            break;
        }
    default:
        ScriptError("ScriptThreadLabel::SetScript: bad label type '%s'", label.GetTypeName());
        break;
    }

    if (m_Script && !m_Script->m_State.FindLabel(m_Label)) {
        const str& scriptName = m_Script->Filename();
        const str& labelName  = Director.GetString(m_Label);

        m_Script = NULL;
        m_Label  = STRING_EMPTY;

        ScriptError("^~^~^ Could not find label '%s' in '%s'", scriptName.c_str(), labelName.c_str());
    }
}

void ScriptThreadLabel::SetScript(const char *label)
{
    str   script;
    char  buffer[1023];
    char *p          = buffer;
    int   i          = 0;
    bool  foundLabel = false;

    if (!label || !*label) {
        m_Script = NULL;
        m_Label  = STRING_EMPTY;
        return;
    }

    Q_strncpyz(buffer, label, sizeof(buffer));

    script = buffer;
    while (true) {
        if (p[0] == ':' && p[1] == ':') {
            *p = '\0';

            m_Label    = Director.AddString(&p[2]);
            foundLabel = true;

            break;
        }

        p++;
        i++;

        if (*p == '\0') {
            m_Label    = STRING_EMPTY;
            foundLabel = true;
            break;
        }

        if (i >= 1023) {
            //we didn't find label so empty label
            m_Label    = STRING_EMPTY;
            foundLabel = false;
            break;
        }
    }

    m_Script = Director.GetGameScript(script);

    if (!m_Script->m_State.FindLabel(m_Label)) {
        str l = Director.GetString(m_Label);

        m_Script = NULL;
        m_Label  = STRING_EMPTY;

        ScriptError("^~^~^ Could not find label '%s' in '%s'", l.c_str(), script.c_str());
    }
}

void ScriptThreadLabel::SetScript(const_str label)
{
    SetScript(Director.GetString(label));
}

void ScriptThreadLabel::SetThread(const ScriptVariable& label)
{
    switch (label.GetType()) {
    case VARIABLE_STRING:
    case VARIABLE_CONSTSTRING:
        m_Script = Director.CurrentScriptClass()->GetScript();
        m_Label  = label.constStringValue();
        break;
    case VARIABLE_NONE:
        m_Script = NULL;
        m_Label  = STRING_EMPTY;
        break;
    case VARIABLE_CONSTARRAY:
        if (label.arraysize() > 1) {
            ScriptVariable *script    = label[1];
            ScriptVariable *labelname = label[2];

            if (script->GetType() == VARIABLE_CONSTSTRING) {
                m_Script = Director.GetGameScript(script->constStringValue());
            } else {
                m_Script = Director.GetGameScript(script->stringValue());
            }

            m_Label = labelname->constStringValue();
            break;
        }
    default:
        ScriptError("ScriptThreadLabel::SetThread: bad label type '%s'", label.GetTypeName());
        break;
    }

    if (m_Script && !m_Script->m_State.FindLabel(m_Label)) {
        const str& scriptName = m_Script->Filename();
        const str& labelName  = Director.GetString(m_Label);

        m_Script = NULL;
        m_Label  = STRING_EMPTY;

        ScriptError("^~^~^ Could not find label '%s' in '%s'", scriptName.c_str(), labelName.c_str());
    }
}

bool ScriptThreadLabel::TrySet(const char *label)
{
    try {
        Set(label);
    } catch (const ScriptException& exc) {
        Com_Printf("%s\n", exc.string.c_str());
        return false;
    }

    return true;
}

bool ScriptThreadLabel::TrySet(const_str label)
{
    try {
        Set(label);
    } catch (const ScriptException& exc) {
        Com_Printf("%s\n", exc.string.c_str());
        return false;
    }

    return true;
}

bool ScriptThreadLabel::TrySetScript(const char *label)
{
    try {
        SetScript(label);
    } catch (const ScriptException& exc) {
        Com_Printf("%s\n", exc.string.c_str());
        return false;
    }

    return true;
}

bool ScriptThreadLabel::TrySetScript(const_str label)
{
    try {
        SetScript(label);
    } catch (const ScriptException& exc) {
        Com_Printf("%s\n", exc.string.c_str());
        return false;
    }

    return true;
}

void ScriptThreadLabel::GetScriptValue(ScriptVariable *var) const
{
    if (!m_Script) {
        var->Clear();
        return;
    }

    ScriptVariable var_array[2];

    var_array[0].setConstStringValue(m_Script->ConstFilename());
    var_array[1].setConstStringValue(m_Label);
    var->setConstArrayValue(var_array, 2);
}

bool ScriptThreadLabel::IsSet(void) const
{
    return m_Script != NULL;
}

bool ScriptThreadLabel::IsFile(const_str filename) const
{
    return m_Script && m_Script->ConstFilename() == filename && m_Label == STRING_EMPTY;
}

void ScriptThreadLabel::Archive(Archiver& arc)
{
    GameScript::Archive(arc, m_Script);

    Director.ArchiveString(arc, m_Label);
}
