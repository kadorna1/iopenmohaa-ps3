/*
===========================================================================
Copyright (C) 2023 the OpenMoHAA team

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

#include "cg_local.h"
#include "cg_archive.h"
#include "cg_commands.h"
#include "memarchiver.h"
#include "../corepp/tiki.h"
#include "../sys/ps3_step.h"

void CG_ArchiveStuff(MemArchiver& archiver, int svsTime)
{
	archiver.SetBaseTime(svsTime);
#if defined(__PS3__)
	if (archiver.IsWriting()) {
		PS3_TRACEF("SAVE CG archive begin bytes=%lu capacity=%lu",
			(unsigned long)archiver.BufferSize(), (unsigned long)archiver.Capacity());
		PS3_TRACEMEM("SAVE CG archive entry");
	}
#endif
	commandManager.ArchiveToMemory(archiver);
#if defined(__PS3__)
	if (archiver.IsWriting()) {
		PS3_TRACEF("SAVE CG command-manager archive complete bytes=%lu capacity=%lu",
			(unsigned long)archiver.BufferSize(), (unsigned long)archiver.Capacity());
		PS3_TRACEMEM("SAVE CG after command manager archive");
	}
#endif
#if defined(__PS3__)
	if (archiver.IsWriting()) {
		PS3_TRACEF("SAVE CG VSS globals begin bytes=%lu capacity=%lu",
			(unsigned long)archiver.BufferSize(), (unsigned long)archiver.Capacity());
	}
#endif
	CG_ArchiveVSSGlobals(archiver);
#if defined(__PS3__)
	if (archiver.IsWriting()) {
		PS3_TRACEF("SAVE CG VSS globals complete bytes=%lu capacity=%lu",
			(unsigned long)archiver.BufferSize(), (unsigned long)archiver.Capacity());
		PS3_TRACEMEM("SAVE CG after VSS globals");
		PS3_TRACEF("SAVE CG archive complete");
	}
#endif
}

size_t CG_SaveStateToBuffer(void** out, int svsTime)
{
	MemArchiver archiver;
	size_t size;

	if (out) {
		*out = NULL;
	}

#if defined(__PS3__)
	// The last checkpoint snapshot reached ~2.47 MiB. Reserve 4 MiB once so
	// growth does not temporarily hold both the old 2 MiB and new 4 MiB buffers.
	archiver.SetupForWriting(0x400000);
#else
	archiver.SetupForWriting(0x200000);
#endif
#if defined(__PS3__)
	PS3_TRACEF("SAVE CG snapshot buffer initialized bytes=%lu capacity=%lu",
		(unsigned long)archiver.BufferSize(), (unsigned long)archiver.Capacity());
	PS3_TRACEMEM("SAVE CG snapshot buffer initialized");
#endif
	if (archiver.Failed()) {
#if defined(__PS3__)
		PS3_TRACEF("SAVE CG snapshot aborted: initial buffer allocation failed");
#endif
		return 0;
	}

	CG_ArchiveStuff(archiver, svsTime);
	if (archiver.Failed()) {
#if defined(__PS3__)
		PS3_TRACEF("SAVE CG snapshot aborted: archive allocation failed partial=%lu",
			(unsigned long)archiver.BufferSize());
#endif
		return 0;
	}

	size = archiver.BufferSize();
	if (size && out) {
		*out = archiver.ConfiscateBuffer();
	}
#if defined(__PS3__)
	PS3_TRACEF("SAVE CG snapshot ready size=%lu", (unsigned long)size);
#endif

	return size;
}

qboolean CG_LoadStateToBuffer(void* state, size_t size, int svsTime)
{
	MemArchiver archiver;

	archiver.SetupForReading((byte*)state, size);
	CG_ArchiveStuff(archiver, svsTime);

	return archiver.FinishedReading();
}

void CG_ArchiveStringContainer(MemArchiver& archiver, Container<str>* container)
{
    int num;
    int i;

    if (archiver.IsReading()) {
        str tmp;

        archiver.ArchiveInteger(&num);
        container->ClearObjectList();

        for (i = 0; i < num; i++) {
            archiver.ArchiveString(&tmp);
            container->AddObject(tmp);
        }
    } else {
#if defined(__PS3__)
        if (archiver.IsWriting()) {
            PS3_STEPF("SAVE CG string-container count begin ptr=%p bytes=%lu",
                (void *)container, (unsigned long)archiver.BufferSize());
        }
#endif
        num = container->NumObjects();
#if defined(__PS3__)
        if (archiver.IsWriting()) {
            PS3_STEPF("SAVE CG string-container count result ptr=%p entries=%d bytes=%lu",
                (void *)container, num, (unsigned long)archiver.BufferSize());
        }
#endif
        archiver.ArchiveInteger(&num);

        for (i = 0; i < num; i++) {
#if defined(__PS3__)
            if (archiver.IsWriting() && ((i & 31) == 0 || i == num - 1)) {
                PS3_STEPF("SAVE CG string-container item begin ptr=%p index=%d count=%d",
                    (void *)container, i, num);
            }
#endif
            str& tmp = container->ObjectAt(i + 1);
            archiver.ArchiveString(&tmp);
        }
#if defined(__PS3__)
        if (archiver.IsWriting()) {
            PS3_STEPF("SAVE CG string-container complete ptr=%p entries=%d bytes=%lu",
                (void *)container, num, (unsigned long)archiver.BufferSize());
        }
#endif
    }
}

void CG_ArchiveTikiPointer(MemArchiver& archiver, dtiki_t** pTiki)
{
    str tmp;

    if (archiver.IsReading()) {
        archiver.ArchiveString(&tmp);
        if (tmp.length()) {
            *pTiki = cgi.R_Model_GetHandle(cgi.R_RegisterModel(tmp.c_str()));
        } else {
            *pTiki = NULL;
        }
    } else {
#if defined(__PS3__)
        if (archiver.IsWriting()) {
            PS3_STEPF("SAVE CG TIKI pointer begin field=%p tiki=%p",
                (void *)pTiki, pTiki ? (void *)*pTiki : NULL);
        }
#endif
        if (*pTiki) {
#if defined(__PS3__)
            if (archiver.IsWriting()) PS3_STEPF("SAVE CG TIKI dereference begin tiki=%p", (void *)*pTiki);
#endif
            tmp = (*pTiki)->name;
#if defined(__PS3__)
            if (archiver.IsWriting()) PS3_STEPF("SAVE CG TIKI dereference complete nameLength=%lu", (unsigned long)tmp.length());
#endif
        }

        archiver.ArchiveString(&tmp);
#if defined(__PS3__)
        if (archiver.IsWriting()) {
            PS3_STEPF("SAVE CG TIKI pointer complete bytes=%lu",
                (unsigned long)archiver.BufferSize());
        }
#endif
    }
}

void CG_ArchiveModelHandle(MemArchiver& archiver, qhandle_t* handle)
{
    str tmp;

	if (archiver.IsReading()) {
		archiver.ArchiveString(&tmp);
		if (tmp.length()) {
			*handle = cgi.R_RegisterModel(tmp.c_str());
		} else {
			*handle = (qhandle_t)0;
		}
	} else {
		if (*handle) {
			tmp = cgi.R_GetModelName(*handle);
		} else {
			tmp = "";
		}

		archiver.ArchiveString(&tmp);
	}
}

void CG_ArchiveShaderHandle(MemArchiver& archiver, qhandle_t* handle)
{
    str tmp;

	if (archiver.IsReading()) {
		archiver.ArchiveString(&tmp);
		if (tmp.length()) {
			*handle = cgi.R_RegisterShader(tmp.c_str());
		} else {
			*handle = (qhandle_t)0;
		}
	} else {
		if (*handle) {
			tmp = cgi.R_GetShaderName(*handle);
		} else {
			tmp = "";
		}

		archiver.ArchiveString(&tmp);
	}
}

void CG_ArchiveRefEntity(MemArchiver& archiver, refEntity_t* ref)
{
    archiver.ArchiveRaw(&ref->reType, sizeof(ref->reType));
    archiver.ArchiveInteger(&ref->renderfx);

    CG_ArchiveModelHandle(archiver, &ref->hModel);
    CG_ArchiveModelHandle(archiver, &ref->hOldModel);

    archiver.ArchiveVec3(ref->lightingOrigin);
    archiver.ArchiveInteger(&ref->parentEntity);
    archiver.ArchiveVec3(ref->axis[0]);
    archiver.ArchiveVec3(ref->axis[1]);
    archiver.ArchiveVec3(ref->axis[2]);
    archiver.ArchiveBoolean(&ref->nonNormalizedAxes);
    archiver.ArchiveVec3(ref->origin);
    archiver.ArchiveRaw(ref->frameInfo, sizeof(ref->frameInfo));
    archiver.ArchiveFloat(&ref->actionWeight);
    archiver.ArchiveShort(&ref->wasframe);
    archiver.ArchiveFloat(&ref->scale);
    archiver.ArchiveVec3(ref->oldorigin);
    archiver.ArchiveInteger(&ref->skinNum);

    CG_ArchiveShaderHandle(archiver, &ref->customShader);
    archiver.ArchiveRaw(ref->shaderRGBA, sizeof(ref->shaderRGBA));
    archiver.ArchiveFloat(ref->shaderTexCoord);
    archiver.ArchiveFloat(&ref->shaderTexCoord[1]);
    archiver.ArchiveFloat(&ref->shaderTime);

    archiver.ArchiveInteger(&ref->entityNumber);
    archiver.ArchiveRaw(ref->surfaces, 32);
    archiver.ArchiveFloat(ref->shader_data);
    archiver.ArchiveFloat(&ref->shader_data[1]);

    ref->bone_tag = NULL;
    ref->bone_quat = NULL;
    ref->of = NULL;
    ref->nf = NULL;

    CG_ArchiveTikiPointer(archiver, &ref->tiki);

    archiver.ArchiveInteger(&ref->bonestart);
    archiver.ArchiveInteger(&ref->morphstart);
    archiver.ArchiveBoolean(&ref->hasMorph);
    archiver.ArchiveFloat(&ref->radius);
    archiver.ArchiveFloat(&ref->rotation);
}
