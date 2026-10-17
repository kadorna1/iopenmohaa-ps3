/*
===========================================================================
Copyright (C) 2024 the OpenMoHAA team

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

// tiki_skel.cpp : TIKI skeletor loader
//
// Some part of the code uses CopyLittleLong / LongNoSwapPtr, etc. because some CPUs can't read misaligned memory
//

#include "q_shared.h"
#include "qcommon.h"
#include "../sys/ps3_step.h"
#include "../skeletor/skeletor.h"
#include "../client/client.h"
#include "../corepp/tiki.h"

#if defined(__PS3__)
extern "C" void Z_CheckHeap(void);

/*
 * Skeletal asset vertex records are packed in the file and can begin at a
 * non-word-aligned address. Although FloatNoSwapPtr() is written using
 * memcpy(), the PPU GCC release build optimizes that helper to an lfs
 * instruction, which faults on this asset's two-byte-aligned float source.
 * Copy the already endian-normalized float representation bytewise instead
 * of loading it through a floating-point register.
 */
static void TIKI_CopyUnalignedFloatBits(void *destination, const void *source)
{
    volatile const unsigned char *src = (volatile const unsigned char *)source;
    unsigned char *dst = (unsigned char *)destination;
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
}
#endif

/*
===============
LoadBoneFromBuffer2
===============
*/
void LoadBoneFromBuffer2(boneFileData_t *fileData, boneData_t *boneData, qboolean ps3TraceCache)
{
    char *newChannelName;
    char *newBoneRefName;
    int   i;

#if defined(__PS3__)
    if (ps3TraceCache) {
        PS3_BootTraceFastf(
            "TIKI_SKEL_CACHE bone-enter name=%.*s parent=%.*s type=%d base=%d channels=%d refs=%d end=%d",
            (int)sizeof(fileData->name), fileData->name,
            (int)sizeof(fileData->parent), fileData->parent,
            (int)fileData->boneType, fileData->ofsBaseData, fileData->ofsChannelNames,
            fileData->ofsBoneNames, fileData->ofsEnd
        );
    }
#endif
    memset(boneData, 0, sizeof(boneData_t));
#if defined(__PS3__)
    if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE bone-before-register-name");
#endif
    boneData->channel  = skeletor_c::m_boneNames.RegisterChannel(fileData->name);
#if defined(__PS3__)
    if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE bone-after-register-name channel=%d", boneData->channel);
#endif
    boneData->boneType = fileData->boneType;

    if (boneData->boneType == SKELBONE_HOSEROT) {
        i = LittleLongPtr(fileData->parent[fileData->ofsBaseData + 4]);

        if (i == 1) {
            boneData->boneType = SKELBONE_HOSEROTPARENT;
        } else if (i == 2) {
            boneData->boneType = SKELBONE_HOSEROTBOTH;
        }
    }

    newChannelName        = (char *)fileData + fileData->ofsChannelNames;
    boneData->numChannels = skelBone_Base::GetNumChannels(boneData->boneType);

#if defined(__PS3__)
    if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE bone-channel-loop count=%d", boneData->numChannels);
#endif
    for (i = 0; i < boneData->numChannels; i++) {
#if defined(__PS3__)
        if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE bone-channel-before index=%d name=%.*s", i, 48, newChannelName);
#endif
        boneData->channelIndex[i] = skeletor_c::m_channelNames.RegisterChannel(newChannelName);
#if defined(__PS3__)
        if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE bone-channel-after index=%d channel=%d", i, boneData->channelIndex[i]);
#endif
        if (boneData->channelIndex[i] < 0) {
            SKEL_Warning("Channel named %s not added. (Bone will not work without it)\n", newChannelName);
            boneData->boneType = SKELBONE_ZERO;
        }

        newChannelName += strlen(newChannelName) + 1;
    }

    newBoneRefName    = (char *)fileData + fileData->ofsBoneNames;
    boneData->numRefs = skelBone_Base::GetNumBoneRefs(boneData->boneType);

#if defined(__PS3__)
    if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE bone-ref-loop count=%d", boneData->numRefs);
#endif
    for (i = 0; i < boneData->numRefs; i++) {
#if defined(__PS3__)
        if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE bone-ref-before index=%d name=%.*s", i, 48, newBoneRefName);
#endif
        boneData->refIndex[i] = skeletor_c::m_boneNames.RegisterChannel(newBoneRefName);
#if defined(__PS3__)
        if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE bone-ref-after index=%d channel=%d", i, boneData->refIndex[i]);
#endif
        newBoneRefName += strlen(newBoneRefName) + 1;
    }

#if defined(__PS3__)
    if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE bone-before-parent");
#endif
    if (!strcmp(fileData->parent, SKEL_BONENAME_WORLD)) {
        boneData->parent = -1;
    } else {
        boneData->parent = skeletor_c::m_boneNames.RegisterChannel(fileData->parent);
    }
#if defined(__PS3__)
    if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE bone-after-parent parent=%d", boneData->parent);
#endif

    switch (boneData->boneType) {
    case SKELBONE_ROTATION:
        {
            float *baseData = (float *)((char *)fileData + fileData->ofsBaseData);
            CopyLittleLong(&boneData->offset[0], &baseData[0]);
            CopyLittleLong(&boneData->offset[1], &baseData[1]);
            CopyLittleLong(&boneData->offset[2], &baseData[2]);
            break;
        }
    case SKELBONE_IKSHOULDER:
        {
            float *baseData = (float *)((char *)fileData + fileData->ofsBaseData);
            CopyLittleLong(&boneData->offset[0], &baseData[4]);
            CopyLittleLong(&boneData->offset[1], &baseData[5]);
            CopyLittleLong(&boneData->offset[2], &baseData[6]);
            break;
        }
    case SKELBONE_IKELBOW:
    case SKELBONE_IKWRIST:
        {
            float *pBaseData = (float *)((char *)fileData + fileData->ofsBaseData);
            vec3_t baseData;
            CopyLittleLong(&baseData[0], &pBaseData[0]);
            CopyLittleLong(&baseData[1], &pBaseData[1]);
            CopyLittleLong(&baseData[2], &pBaseData[2]);
            boneData->length = VectorLength(baseData);
            break;
        }
    case SKELBONE_AVROT:
        {
            float *baseData = (float *)((char *)fileData + fileData->ofsBaseData);
            CopyLittleLong(&boneData->length, &baseData[0]);
            CopyLittleLong(&boneData->offset[0], &baseData[1]);
            CopyLittleLong(&boneData->offset[1], &baseData[2]);
            CopyLittleLong(&boneData->offset[2], &baseData[3]);
            break;
        }
    case SKELBONE_HOSEROT:
    case SKELBONE_HOSEROTBOTH:
    case SKELBONE_HOSEROTPARENT:
        {
            float *baseData = (float *)((char *)fileData + fileData->ofsBaseData);
            CopyLittleLong(&boneData->offset[0], &baseData[3]);
            CopyLittleLong(&boneData->offset[1], &baseData[4]);
            CopyLittleLong(&boneData->offset[2], &baseData[5]);
            CopyLittleLong(&boneData->bendRatio, &baseData[0]);
            CopyLittleLong(&boneData->bendMax, &baseData[1]);
            CopyLittleLong(&boneData->spinRatio, &baseData[2]);
            break;
        }
    default:
        break;
    }
}

/*
===============
TIKI_CacheFileSkel
===============
*/
void TIKI_CacheFileSkel(skelHeader_t *pHeader, skelcache_t *cache, int length)
{
#if defined(__PS3__)
    const qboolean ps3TraceCache = cache && (strstr(cache->path, "grenade_box") != NULL || strstr(cache->path, "item_grenade_ammobox") != NULL);
    if (ps3TraceCache) {
        PS3_BootTraceFastf(
            "TIKI_SKEL_CACHE enter path=%s bytes=%d version=%d surfaces=%d bones=%d boxes=%d morphs=%d ofsSurf=%d ofsBones=%d ofsBoxes=%d ofsMorphs=%d ofsEnd=%d",
            cache->path, length, pHeader->version, pHeader->numSurfaces, pHeader->numBones,
            pHeader->numBoxes, pHeader->numMorphTargets, pHeader->ofsSurfaces, pHeader->ofsBones,
            pHeader->ofsBoxes, pHeader->ofsMorphTargets, pHeader->ofsEnd
        );
    }
#else
    const qboolean ps3TraceCache = qfalse;
#endif
    skelHeaderGame_t  *pSkel;
    skelSurfaceGame_t *pGameSurf;
    skelSurface_t     *pSurf;
    int                i, j, k;
    size_t             nSurfBytes;
    size_t             nBoneBytes;
    size_t             nBoxBytes;
    size_t             nMorphBytes;
    size_t             nVertBytes;
    size_t             nTriBytes;
    byte              *start_gs_ptr;
    byte              *max_gs_ptr;
    byte              *gs_ptr = NULL;

    pSurf      = (skelSurface_t *)((byte *)pHeader + pHeader->ofsSurfaces);
    nSurfBytes = 0;

    for (i = 0; i < pHeader->numSurfaces; i++) {
        skeletorVertex_t *pVert    = (skeletorVertex_t *)((byte *)pSurf + LongNoSwapPtr(&pSurf->ofsVerts));
        const int         numVerts = LongNoSwapPtr(&pSurf->numVerts);

#if defined(__PS3__)
        if (ps3TraceCache) PS3_BootTraceFastf(
            "TIKI_SKEL_CACHE measure-surface-enter index=%d/%d name=%.*s verts=%d tris=%d ofsVerts=%d ofsTri=%d ofsCollapse=%d ofsCollapseIndex=%d ofsEnd=%d",
            i + 1, pHeader->numSurfaces, (int)sizeof(pSurf->name), pSurf->name,
            numVerts, LongNoSwapPtr(&pSurf->numTriangles), LongNoSwapPtr(&pSurf->ofsVerts),
            LongNoSwapPtr(&pSurf->ofsTriangles), LongNoSwapPtr(&pSurf->ofsCollapse),
            LongNoSwapPtr(&pSurf->ofsCollapseIndex), LongNoSwapPtr(&pSurf->ofsEnd)
        );
#endif
        nVertBytes = 0;
        for (j = 0; j < numVerts; j++) {
            const int numMorphs  = LongNoSwapPtr(&pVert->numMorphs);
            const int numWeights = LongNoSwapPtr(&pVert->numWeights);
#if defined(__PS3__)
            if (ps3TraceCache && (j < 4 || (j % 64) == 0 || j == numVerts - 1)) {
                PS3_BootTraceFastf("TIKI_SKEL_CACHE measure-vertex index=%d/%d morphs=%d weights=%d bytes=%d", j, numVerts, numMorphs, numWeights, (int)nVertBytes);
            }
#endif

            int iOffset = sizeof(skeletorMorph_t) * numMorphs;
            iOffset += sizeof(skelWeight_t) * numWeights;
            iOffset += sizeof(skeletorVertex_t);

            nVertBytes += iOffset;
            pVert = (skeletorVertex_t *)((byte *)pVert + iOffset);
        }

        nSurfBytes += sizeof(skelSurfaceGame_t);
        // triangles
        nSurfBytes += LongNoSwapPtr(&pSurf->numTriangles) * sizeof(skelIndex_t) * 3;
        nSurfBytes = PAD(nSurfBytes, sizeof(void *));
        // vertices
        nSurfBytes += nVertBytes;
        nSurfBytes = PAD(nSurfBytes, sizeof(void *));
        // collapse
        nSurfBytes += numVerts * sizeof(skelIndex_t);

        if (pHeader->version > TIKI_SKB_HEADER_VER_3) {
            // collapse indices
            nSurfBytes += numVerts * sizeof(skelIndex_t);
        }
#if defined(__PS3__)
        if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE measure-surface-exit index=%d verts=%d vertBytes=%d surfBytes=%d", i + 1, numVerts, (int)nVertBytes, (int)nSurfBytes);
#endif

        pSurf = (skelSurface_t *)((byte *)pSurf + LongNoSwapPtr(&pSurf->ofsEnd));
    }

    nBoneBytes  = pHeader->numBones * sizeof(boneData_t);
    nBoxBytes   = 0;
    nMorphBytes = 0;

    if (pHeader->version > TIKI_SKB_HEADER_VERSION) {
        char    *pMorphTargets;
        intptr_t nLen;

        nBoxBytes     = pHeader->numBoxes * sizeof(skelHitBox_t);
        pMorphTargets = (char *)((byte *)pHeader + pHeader->ofsMorphTargets);

        if (pHeader->ofsMorphTargets > 0 || (pHeader->ofsMorphTargets + pHeader->numMorphTargets) < length) {
            for (i = 0; i < pHeader->numMorphTargets; i++) {
                nLen = strlen(pMorphTargets) + 1;
                nMorphBytes += nLen;
                pMorphTargets += nLen;
            }
        } else {
            nMorphBytes = pHeader->numMorphTargets;
        }
    } else if (pHeader->version == TIKI_SKB_HEADER_VERSION) {
        nBoxBytes = pHeader->numBoxes * sizeof(skelHitBox_t);
    }

    cache->size = sizeof(skelHeaderGame_t);
    cache->size += nSurfBytes;
    cache->size = PAD(cache->size, sizeof(void *));
    cache->size += nBoneBytes;
    cache->size = PAD(cache->size, sizeof(void *));
    cache->size += nBoxBytes;
    cache->size = PAD(cache->size, sizeof(void *));
    cache->size += nMorphBytes;
#if defined(__PS3__)
    if (ps3TraceCache) {
        PS3_BootTraceFastf("TIKI_SKEL_CACHE heap-check-before-allocation size=%d", (int)cache->size);
        Z_CheckHeap();
        PS3_BootTraceFastf("TIKI_SKEL_CACHE heap-check-before-allocation-exit");
        PS3_BootTraceFastf("TIKI_SKEL_CACHE allocation-enter size=%d surf=%d bone=%d box=%d morph=%d", (int)cache->size, (int)nSurfBytes, (int)nBoneBytes, (int)nBoxBytes, (int)nMorphBytes);
    }
#endif

    cache->skel = pSkel = (skelHeaderGame_t *)TIKI_Alloc(cache->size);
#if defined(__PS3__)
    if (ps3TraceCache) {
        PS3_BootTraceFastf("TIKI_SKEL_CACHE allocation-exit size=%d ptr=%p end=%p", (int)cache->size, (void *)pSkel, (void *)((byte *)pSkel + cache->size));
        Z_CheckHeap();
        PS3_BootTraceFastf("TIKI_SKEL_CACHE heap-check-after-allocation-exit");
    }
#endif
    byte *start_ptr     = (byte *)pSkel;
    byte *max_ptr       = start_ptr + cache->size;
    byte *ptr           = start_ptr + sizeof(skelHeaderGame_t);

    pSkel->version     = pHeader->version;

    //
    // Added in 2.0
    //
    if (pHeader->version >= TIKI_SKD_HEADER_VERSION) {
        pSkel->scale = pHeader->scale * 0.52;
    } else {
        pSkel->scale = 0.52;
    }

    pSkel->numSurfaces = pHeader->numSurfaces;
    pSkel->numBones    = pHeader->numBones;
    pSkel->pSurfaces   = (skelSurfaceGame_t *)ptr;
    ptr += nSurfBytes;
    ptr           = (byte *)PADP(ptr, sizeof(void *));
    pSkel->pBones = (boneData_t *)ptr;
    ptr += nBoneBytes;
    ptr             = (byte *)PADP(ptr, sizeof(void *));
    pSkel->numBoxes = pHeader->numBoxes;
    pSkel->pBoxes   = (skelHitBox_t *)ptr;
    ptr += nBoxBytes;
    ptr                    = (byte *)PADP(ptr, sizeof(void *));
    pSkel->pLOD            = NULL;
    pSkel->numMorphTargets = pHeader->numMorphTargets;
    pSkel->pMorphTargets   = (char *)ptr;
    ptr += nMorphBytes;
    ptr = (byte *)PADP(ptr, sizeof(void *));
    memcpy(pSkel->name, pHeader->name, sizeof(pSkel->name));

    for (i = 0; i < sizeof(pSkel->lodIndex) / sizeof(pSkel->lodIndex[0]); i++) {
        pSkel->lodIndex[i] = LongNoSwapPtr(&pHeader->lodIndex[i]);
    }

    pSurf     = (skelSurface_t *)((byte *)pHeader + pHeader->ofsSurfaces);
    pGameSurf = pSkel->pSurfaces;

    for (i = 0; i < pHeader->numSurfaces; i++) {
        size_t            nBytesUsed;
        skeletorVertex_t *pVert;
        const int         numTriangles = LongNoSwapPtr(&pSurf->numTriangles);
        const int         numVerts     = LongNoSwapPtr(&pSurf->numVerts);
#if defined(__PS3__)
        if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE convert-surface-enter index=%d/%d verts=%d tris=%d", i + 1, pHeader->numSurfaces, numVerts, numTriangles);
#endif

        nTriBytes  = numTriangles * sizeof(skelIndex_t) * 3;
        nSurfBytes = 0;
        if (pHeader->version > TIKI_SKB_HEADER_VER_3) {
            nSurfBytes = numVerts * sizeof(skelIndex_t);
        }

        pVert = (skeletorVertex_t *)((byte *)pSurf + LongNoSwapPtr(&pSurf->ofsVerts));

        nVertBytes = 0;
        for (j = 0; j < numVerts; j++) {
            const int numMorphs  = LongNoSwapPtr(&pVert->numMorphs);
            const int numWeights = LongNoSwapPtr(&pVert->numWeights);

            int iOffset = sizeof(skeletorMorph_t) * numMorphs;
            iOffset += sizeof(skelWeight_t) * numWeights;
            iOffset += sizeof(skeletorVertex_t);

            nVertBytes += iOffset;
            pVert = (skeletorVertex_t *)((byte *)pVert + iOffset);
        }

        nBytesUsed = sizeof(skelSurfaceGame_t);
        nBytesUsed += nTriBytes;
        nBytesUsed = PAD(nBytesUsed, sizeof(void *));
        nBytesUsed += nVertBytes;
        nBytesUsed = PAD(nBytesUsed, sizeof(void *));
        nBytesUsed += nSurfBytes;

        start_gs_ptr = (byte *)pGameSurf;
        max_gs_ptr   = (byte *)pGameSurf + nBytesUsed;
        gs_ptr       = start_gs_ptr + sizeof(skelSurfaceGame_t);

        pGameSurf->ident            = LongNoSwapPtr(&pSurf->ident);
        pGameSurf->numTriangles     = numTriangles;
        pGameSurf->numVerts         = numVerts;
        pGameSurf->pStaticXyz       = NULL;
        pGameSurf->pStaticNormal    = NULL;
        pGameSurf->pStaticTexCoords = NULL;
        pGameSurf->pTriangles       = (skelIndex_t *)gs_ptr;
        gs_ptr += nTriBytes;
        gs_ptr            = (byte *)PADP(gs_ptr, sizeof(void *));
        pGameSurf->pVerts = (skeletorVertex_t *)gs_ptr;
        gs_ptr += nVertBytes;
        gs_ptr               = (byte *)PADP(gs_ptr, sizeof(void *));
        pGameSurf->pCollapse = (skelIndex_t *)gs_ptr;
        gs_ptr += sizeof(*pGameSurf->pCollapse) * numVerts;
        if (pHeader->version > TIKI_SKB_HEADER_VER_3) {
            pGameSurf->pCollapseIndex = (skelIndex_t *)gs_ptr;
            gs_ptr += sizeof(*pGameSurf->pCollapseIndex) * numVerts;
        }

        memcpy(pGameSurf->name, pSurf->name, sizeof(pGameSurf->name));

#if defined(__PS3__)
        if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE triangles-enter count=%d", numTriangles);
#endif
        if (pGameSurf->numTriangles) {
            const int *pTriangles = (const int *)((byte *)pSurf + LongNoSwapPtr(&pSurf->ofsTriangles));
            for (j = 0; j < numTriangles; j++) {
                pGameSurf->pTriangles[j * 3 + 0] = LongNoSwapPtr(&pTriangles[j * 3 + 0]);
                pGameSurf->pTriangles[j * 3 + 1] = LongNoSwapPtr(&pTriangles[j * 3 + 1]);
                pGameSurf->pTriangles[j * 3 + 2] = LongNoSwapPtr(&pTriangles[j * 3 + 2]);
            }
        } else {
            pGameSurf->pTriangles = NULL;
        }
#if defined(__PS3__)
        if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE triangles-exit");
#endif

#if defined(__PS3__)
        if (ps3TraceCache) {
            PS3_BootTraceFastf("TIKI_SKEL_CACHE vertices-enter count=%d source=%p destination=%p sourceOffset=%d destinationOffset=%d vertexBytes=%d allocation=%d", pGameSurf->numVerts, (void *)((byte *)pSurf + LongNoSwapPtr(&pSurf->ofsVerts)), (void *)pGameSurf->pVerts, (int)(((byte *)pSurf + LongNoSwapPtr(&pSurf->ofsVerts)) - (byte *)pHeader), (int)((byte *)pGameSurf->pVerts - (byte *)pSkel), (int)nVertBytes, (int)cache->size);
            PS3_BootTraceFastf("TIKI_SKEL_CACHE heap-check-before-vertices");
            Z_CheckHeap();
            PS3_BootTraceFastf("TIKI_SKEL_CACHE heap-check-before-vertices-exit");
        }
#endif
        if (pGameSurf->numVerts) {
            skeletorVertex_t *skelVert = pGameSurf->pVerts;
            pVert                      = (skeletorVertex_t *)((char *)pSurf + LongNoSwapPtr(&pSurf->ofsVerts));

            for (j = 0; j < pGameSurf->numVerts; j++) {
                const int numMorphs  = LongNoSwapPtr(&pVert->numMorphs);
                const int numWeights = LongNoSwapPtr(&pVert->numWeights);
#if defined(__PS3__)
                if (ps3TraceCache && (j < 4 || (j % 64) == 0 || j == pGameSurf->numVerts - 1)) {
                    PS3_BootTraceFastf("TIKI_SKEL_CACHE convert-vertex index=%d/%d morphs=%d weights=%d", j, pGameSurf->numVerts, numMorphs, numWeights);
                }
                if (ps3TraceCache && j == 0) {
                    PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-addresses src=%p dst=%p srcOffset=%d dstOffset=%d allocation=%d", (void *)pVert, (void *)skelVert, (int)((byte *)pVert - (byte *)pHeader), (int)((byte *)skelVert - (byte *)pSkel), (int)cache->size);
                    PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-size-enter morphSize=%d weightSize=%d vertexSize=%d", (int)sizeof(skeletorMorph_t), (int)sizeof(skelWeight_t), (int)sizeof(skeletorVertex_t));
                }
#endif

                int iOffset = sizeof(skeletorMorph_t) * numMorphs;
                iOffset += sizeof(skelWeight_t) * numWeights;
                iOffset += sizeof(skeletorVertex_t);
#if defined(__PS3__)
                if (ps3TraceCache && j == 0) PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-offset-computed offset=%d sourceRemain=%d destinationRemain=%d", iOffset, (int)(length - ((byte *)pVert - (byte *)pHeader)), (int)(cache->size - ((byte *)skelVert - (byte *)pSkel)));
#endif

#if defined(__PS3__)
                if (ps3TraceCache && j == 0) PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-normal-read-enter src=%p dst=%p", (void *)&pVert->normal[0], (void *)&skelVert->normal[0]);
                TIKI_CopyUnalignedFloatBits(&skelVert->normal[0], &pVert->normal[0]);
                if (ps3TraceCache && j == 0) PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-normal-0-copy-exit");
                TIKI_CopyUnalignedFloatBits(&skelVert->normal[1], &pVert->normal[1]);
                if (ps3TraceCache && j == 0) PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-normal-1-copy-exit");
                TIKI_CopyUnalignedFloatBits(&skelVert->normal[2], &pVert->normal[2]);
                if (ps3TraceCache && j == 0) PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-normal-2-copy-exit");
                TIKI_CopyUnalignedFloatBits(&skelVert->texCoords[0], &pVert->texCoords[0]);
                if (ps3TraceCache && j == 0) PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-uv-0-copy-exit");
                TIKI_CopyUnalignedFloatBits(&skelVert->texCoords[1], &pVert->texCoords[1]);
                if (ps3TraceCache && j == 0) PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-uv-1-copy-exit");
                if (ps3TraceCache && j == 0) PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-normal-read-exit");
#else
                skelVert->normal[0]    = FloatNoSwapPtr(&pVert->normal[0]);
                skelVert->normal[1]    = FloatNoSwapPtr(&pVert->normal[1]);
                skelVert->normal[2]    = FloatNoSwapPtr(&pVert->normal[2]);
                skelVert->texCoords[0] = FloatNoSwapPtr(&pVert->texCoords[0]);
                skelVert->texCoords[1] = FloatNoSwapPtr(&pVert->texCoords[1]);
#endif
                skelVert->numWeights   = numWeights;
                skelVert->numMorphs    = numMorphs;
#if defined(__PS3__)
                if (ps3TraceCache && j == 0) PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-base-copy-exit normal=%f,%f,%f uv=%f,%f weights=%d morphs=%d", skelVert->normal[0], skelVert->normal[1], skelVert->normal[2], skelVert->texCoords[0], skelVert->texCoords[1], skelVert->numWeights, skelVert->numMorphs);
#endif

                // copy morphs
                skeletorMorph_t *pMorph    = (skeletorMorph_t *)((byte *)pVert + sizeof(skeletorVertex_t));
                skeletorMorph_t *skelMorph = (skeletorMorph_t *)((byte *)skelVert + sizeof(skeletorVertex_t));
                for (k = 0; k < numMorphs; k++, pMorph++, skelMorph++) {
                    skelMorph->morphIndex = LongNoSwapPtr(&pMorph->morphIndex);
                    #if defined(__PS3__)
                    TIKI_CopyUnalignedFloatBits(&skelMorph->offset[0], &pMorph->offset[0]);
                    #else
                    skelMorph->offset[0] = FloatNoSwapPtr(&pMorph->offset[0]);
                    #endif
                    #if defined(__PS3__)
                    TIKI_CopyUnalignedFloatBits(&skelMorph->offset[1], &pMorph->offset[1]);
                    #else
                    skelMorph->offset[1] = FloatNoSwapPtr(&pMorph->offset[1]);
                    #endif
                    #if defined(__PS3__)
                    TIKI_CopyUnalignedFloatBits(&skelMorph->offset[2], &pMorph->offset[2]);
                    #else
                    skelMorph->offset[2] = FloatNoSwapPtr(&pMorph->offset[2]);
                    #endif
                }

                skelWeight_t *pWeight    = (skelWeight_t *)((byte *)pMorph);
                skelWeight_t *skelWeight = (skelWeight_t *)((byte *)skelMorph);
#if defined(__PS3__)
                if (ps3TraceCache && j == 0) PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-weight-loop-enter count=%d source=%p destination=%p", numWeights, (void *)pWeight, (void *)skelWeight);
#endif
                for (k = 0; k < numWeights; k++, skelWeight++, pWeight++) {
#if defined(__PS3__)
                    if (ps3TraceCache && j == 0) PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-weight-enter index=%d source=%p destination=%p", k, (void *)pWeight, (void *)skelWeight);
#endif
                    skelWeight->boneIndex  = LongNoSwapPtr(&pWeight->boneIndex);
#if defined(__PS3__)
                    if (ps3TraceCache && j == 0) PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-weight-bone-index value=%d", skelWeight->boneIndex);
#endif
                    #if defined(__PS3__)
                    TIKI_CopyUnalignedFloatBits(&skelWeight->boneWeight, &pWeight->boneWeight);
                    #else
                    skelWeight->boneWeight = FloatNoSwapPtr(&pWeight->boneWeight);
                    #endif
#if defined(__PS3__)
                    if (ps3TraceCache && j == 0) PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-weight-weight value=%f", skelWeight->boneWeight);
#endif
                    #if defined(__PS3__)
                    TIKI_CopyUnalignedFloatBits(&skelWeight->offset[0], &pWeight->offset[0]);
                    #else
                    skelWeight->offset[0] = FloatNoSwapPtr(&pWeight->offset[0]);
                    #endif
                    #if defined(__PS3__)
                    TIKI_CopyUnalignedFloatBits(&skelWeight->offset[1], &pWeight->offset[1]);
                    #else
                    skelWeight->offset[1] = FloatNoSwapPtr(&pWeight->offset[1]);
                    #endif
                    #if defined(__PS3__)
                    TIKI_CopyUnalignedFloatBits(&skelWeight->offset[2], &pWeight->offset[2]);
                    #else
                    skelWeight->offset[2] = FloatNoSwapPtr(&pWeight->offset[2]);
                    #endif
#if defined(__PS3__)
                    if (ps3TraceCache && j == 0) PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-weight-exit offset=%f,%f,%f", skelWeight->offset[0], skelWeight->offset[1], skelWeight->offset[2]);
#endif
                }
#if defined(__PS3__)
                if (ps3TraceCache && j == 0) PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-weight-loop-exit");
#endif

                pVert    = (skeletorVertex_t *)((byte *)pVert + iOffset);
                skelVert = (skeletorVertex_t *)((byte *)skelVert + iOffset);
#if defined(__PS3__)
                if (ps3TraceCache && j == 0) {
                    PS3_BootTraceFastf("TIKI_SKEL_CACHE vertex-pointer-advance-exit srcOffset=%d dstOffset=%d", (int)((byte *)pVert - (byte *)pHeader), (int)((byte *)skelVert - (byte *)pSkel));
                    PS3_BootTraceFastf("TIKI_SKEL_CACHE convert-vertex-exit index=0 nextSource=%p nextDestination=%p", (void *)pVert, (void *)skelVert);
                    Z_CheckHeap();
                    PS3_BootTraceFastf("TIKI_SKEL_CACHE heap-check-after-first-vertex-exit");
                }
#endif
            }

            assert((byte *)skelVert - (byte *)pGameSurf <= (byte *)pGameSurf->pCollapse - (byte *)pGameSurf);
#if defined(__PS3__)
            if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE collapse-enter count=%d", numVerts);
#endif

            const int *pCollapse = (const int *)((byte *)pSurf + LongNoSwapPtr(&pSurf->ofsCollapse));
            for (j = 0; j < numVerts; j++) {
                pGameSurf->pCollapse[j] = LongNoSwapPtr(&pCollapse[j]);
            }

            if (pHeader->version > TIKI_SKB_HEADER_VER_3) {
                const int *pCollapseIndex = (const int *)((byte *)pSurf + LongNoSwapPtr(&pSurf->ofsCollapseIndex));
                for (j = 0; j < numVerts; j++) {
                    pGameSurf->pCollapseIndex[j] = LongNoSwapPtr(&pCollapseIndex[j]);
                }
            } else {
                pGameSurf->pCollapseIndex = NULL;
            }
#if defined(__PS3__)
            if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE collapse-exit");
#endif
        } else {
            pGameSurf->pVerts         = NULL;
            pGameSurf->pCollapse      = NULL;
            pGameSurf->pCollapseIndex = NULL;
        }

#if defined(__PS3__)
        if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE convert-surface-exit index=%d/%d", i + 1, pHeader->numSurfaces);
#endif
        if (i != pHeader->numSurfaces - 1) {
            pGameSurf->pNext = (skelSurfaceGame_t *)gs_ptr;
            pGameSurf        = pGameSurf->pNext;
            pSurf            = (skelSurface_t *)((byte *)pSurf + LongNoSwapPtr(&pSurf->ofsEnd));
        }
    }

    pGameSurf->pNext = NULL;

    assert(((byte *)pGameSurf - (byte *)pSkel) <= cache->size);

#if defined(__PS3__)
    if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE bones-enter bytes=%d count=%d ofs=%d version=%d", (int)nBoneBytes, pSkel->numBones, LongNoSwapPtr(&pHeader->ofsBones), pHeader->version);
#endif
    if (nBoneBytes) {
        if (pHeader->version <= TIKI_SKB_HEADER_VERSION) {
            skelBoneName_t *TIKI_bones = (skelBoneName_t *)((byte *)pHeader + LongNoSwapPtr(&pHeader->ofsBones));
            for (i = 0; i < pSkel->numBones; i++) {
                const char *boneName;

                if (TIKI_bones->parent == -1) {
                    boneName = SKEL_BONENAME_WORLD;
                } else {
                    boneName = TIKI_bones[TIKI_bones->parent].name;
                }

                CreatePosRotBoneData(TIKI_bones->name, boneName, &pSkel->pBones[i]);
                TIKI_bones++;
            }
        } else {
            boneFileData_t *boneBuffer = (boneFileData_t *)((byte *)pHeader + LongNoSwapPtr(&pHeader->ofsBones));
            for (i = 0; i < pSkel->numBones; i++) {
#if defined(__PS3__)
                if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE bone-convert-enter index=%d/%d offset=%d", i + 1, pSkel->numBones, (int)((byte *)boneBuffer - (byte *)pHeader));
#endif
                LoadBoneFromBuffer2(boneBuffer, &pSkel->pBones[i], ps3TraceCache);
#if defined(__PS3__)
                if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE bone-convert-exit index=%d/%d nextOffset=%d", i + 1, pSkel->numBones, (int)((byte *)boneBuffer - (byte *)pHeader) + LongNoSwapPtr(&boneBuffer->ofsEnd));
#endif
                boneBuffer = (boneFileData_t *)((byte *)boneBuffer + LongNoSwapPtr(&boneBuffer->ofsEnd));
            }
        }
    } else {
        pSkel->numBones = 0;
        pSkel->pBones   = NULL;
    }
#if defined(__PS3__)
    if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE bones-exit");
#endif

    if (pHeader->version <= TIKI_SKB_HEADER_VER_3) {
        pSkel->numBoxes        = 0;
        pSkel->pBoxes          = NULL;
        pSkel->numMorphTargets = 0;
        pSkel->pMorphTargets   = NULL;
#if defined(__PS3__)
        if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE exit version3-path");
#endif
        return;
    }

    if (nBoxBytes) {
        if (pHeader->ofsBoxes <= 0 || (nBoxBytes + pHeader->ofsBoxes) > length) {
            Com_Printf("^~^~^ Box data is corrupted for '%s'\n", cache->path);
            pSkel->numBoxes = 0;
            pSkel->pBoxes   = NULL;
        } else {
            memcpy(pSkel->pBoxes, ((byte *)pHeader + pHeader->ofsBoxes), nBoxBytes);
        }
    } else {
        pSkel->numBoxes = 0;
        pSkel->pBoxes   = NULL;
    }

    if (pHeader->version <= TIKI_SKB_HEADER_VERSION) {
        pSkel->numMorphTargets = 0;
        pSkel->pMorphTargets   = NULL;
#if defined(__PS3__)
        if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE exit no-morph-path");
#endif
        return;
    }

    if (nMorphBytes) {
        if (pHeader->ofsMorphTargets <= 0 || (nMorphBytes + pHeader->ofsMorphTargets) > length) {
            Com_Printf("^~^~^ Morph targets data is corrupted for '%s'\n", cache->path);
            pSkel->numMorphTargets = 0;
            pSkel->pMorphTargets   = NULL;
        } else {
            memcpy(pSkel->pMorphTargets, ((byte *)pHeader + pHeader->ofsMorphTargets), nMorphBytes);
        }
    } else {
        pSkel->numMorphTargets = 0;
        pSkel->pMorphTargets   = NULL;
    }
#if defined(__PS3__)
    if (ps3TraceCache) PS3_BootTraceFastf("TIKI_SKEL_CACHE exit");
#endif
}

/*
===============
TIKI_SortLOD
===============
*/
void TIKI_SortLOD(skelHeaderGame_t *skelmodel)
{
    skelSurfaceGame_t *surf;
    int                render_count;
    int                nTriVerts;
    int                i;
    int                collapse[TIKI_MAX_VERTEXES];
    int                iTemp;

    for (surf = skelmodel->pSurfaces; surf != NULL; surf = surf->pNext) {
        for (i = 0; i < surf->numVerts; i++) {
            collapse[i] = i;
        }

        render_count = surf->numTriangles * 3;

        nTriVerts = surf->numVerts - 1;
        while (nTriVerts > 1) {
            for (i = nTriVerts; i > 1; i--) {
                if (surf->pCollapseIndex[i - 1] != surf->pCollapseIndex[i]) {
                    break;
                }
            }

            nTriVerts = i - 1;
            for (; i < surf->numVerts; i++) {
                collapse[i] = collapse[surf->pCollapse[i]];
            }

            i = 0;
            while (i < render_count) {
                if (collapse[surf->pTriangles[i]] != collapse[surf->pTriangles[i + 1]]
                    && collapse[surf->pTriangles[i + 1]] != collapse[surf->pTriangles[i + 2]]
                    && collapse[surf->pTriangles[i + 2]] != collapse[surf->pTriangles[i]]) {
                    i += 3;
                } else {
                    render_count -= 3;

                    iTemp                          = surf->pTriangles[i];
                    surf->pTriangles[i]            = surf->pTriangles[render_count];
                    surf->pTriangles[render_count] = iTemp;

                    iTemp                              = surf->pTriangles[i + 1];
                    surf->pTriangles[i + 1]            = surf->pTriangles[render_count + 1];
                    surf->pTriangles[render_count + 1] = iTemp;

                    iTemp                              = surf->pTriangles[i + 2];
                    surf->pTriangles[i + 2]            = surf->pTriangles[render_count + 2];
                    surf->pTriangles[render_count + 2] = iTemp;
                }
            }
        }
    }
}

/*
===============
TIKI_LoadSKB
===============
*/
qboolean TIKI_LoadSKB(const char *path, skelcache_t *cache)
{
#if defined(__PS3__)
    const qboolean ps3TraceAmmobox = path && (strstr(path, "item_grenade_ammobox") != NULL || strstr(path, "grenade_box") != NULL);
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKB enter path=%s", path);
#endif
    int            i, j, k;
    skelHeader_t  *pheader;
    skelSurface_t *surf;
    int            version;
    unsigned int   header;
    int            length;
    char          *buf;
    int            totalVerts;
    int            newLength;
    skelHeader_t  *newHeader;
    skelSurface_t *oldSurf;
    skelSurface_t *newSurf;

#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKB before-read path=%s", path);
#endif
    length = TIKI_ReadFileEx(path, (void **)&buf, true);
#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKB after-read path=%s length=%d buffer=%p", path, length, (void *)buf);
#endif
    if (length < 0) {
        TIKI_DPrintf("Tiki:LoadAnim Couldn't load %s\n", path);
        return qfalse;
    }

    pheader = (skelHeader_t *)TIKI_Alloc(length);
    memcpy(pheader, buf, length);
    TIKI_FreeFile(buf);
    memset(cache, 0, sizeof(skelcache_t));
    strncpy(cache->path, path, sizeof(cache->path));

    header = pheader->ident;
    if (header != TIKI_SKB_HEADER_IDENT) {
        TIKI_Error("TIKI_LoadSKB: Tried to load '%s' as a skeletal base frame (File has invalid header)\n", path);
        TIKI_Free(pheader);
        return qfalse;
    }

    version = LittleLong(pheader->version);
    if (version != TIKI_SKB_HEADER_VER_3 && version != TIKI_SKB_HEADER_VERSION) {
        TIKI_Error(
            "TIKI_LoadSKB: %s has wrong version (%i should be %i or %i)\n",
            path,
            version,
            TIKI_SKB_HEADER_VER_3,
            TIKI_SKB_HEADER_VERSION
        );
        TIKI_Free(pheader);
        return qfalse;
    }

#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKB header path=%s version=%d surfaces=%d bones=%d bytes=%d", path, version, LittleLong(pheader->numSurfaces), LittleLong(pheader->numBones), length);
#endif
    TIKI_SwapSkel(pheader);
#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKB after-swap path=%s surfaces=%d bones=%d", path, pheader->numSurfaces, pheader->numBones);
#endif

    surf = (skelSurface_t *)((char *)pheader + pheader->ofsSurfaces);
    for (i = 0; i < pheader->numSurfaces; i++) {
        int numSurfVerts, numSurfTriangles;

        numSurfVerts     = LongNoSwapPtr(&surf->numVerts);
        numSurfTriangles = LongNoSwapPtr(&surf->numTriangles);

        if (numSurfVerts > TIKI_MAX_VERTEXES) {
            TIKI_Error(
                "TIKI_LoadSKB: %s has more than %i verts on a surface (%i)", path, TIKI_MAX_VERTEXES, numSurfVerts
            );
            TIKI_Free(pheader);
            return qfalse;
        }

        if (numSurfTriangles > TIKI_MAX_TRIANGLES) {
            TIKI_Error(
                "TIKI_LoadSKB: %s has more than %i triangles on a surface (%i)",
                path,
                TIKI_MAX_TRIANGLES,
                numSurfTriangles
            );
            TIKI_Free(pheader);
            return qfalse;
        }

        surf = (skelSurface_t *)((char *)surf + LongNoSwapPtr(&surf->ofsEnd));
    }

    if (pheader->numBones > TIKI_MAX_BONES) {
        TIKI_Error("TIKI_LoadSKB: %s has more than %i bones (%i)\n", path, TIKI_MAX_BONES, pheader->numBones);
        TIKI_Free(pheader);
        return qfalse;
    }

    totalVerts = 0;
    surf       = (skelSurface_t *)((byte *)pheader + pheader->ofsSurfaces);
    for (i = 0; i < pheader->numSurfaces; i++) {
        totalVerts += LongNoSwapPtr(&surf->numVerts);
        surf = (skelSurface_t *)((byte *)surf + LongNoSwapPtr(&surf->ofsEnd));
    }

    newLength = totalVerts * sizeof(unsigned int);
    newHeader = (skelHeader_t *)TIKI_Alloc(length + newLength);
    memcpy(newHeader, pheader, pheader->ofsSurfaces);

    if (newHeader->ofsBones > newHeader->ofsSurfaces) {
        newHeader->ofsBones += newLength;
    }
    if (newHeader->ofsBoxes > newHeader->ofsSurfaces) {
        newHeader->ofsBoxes += newLength;
    }
    if (newHeader->ofsEnd > newHeader->ofsSurfaces) {
        newHeader->ofsEnd += newLength;
    }

    oldSurf = (skelSurface_t *)((byte *)pheader + pheader->ofsSurfaces);
    newSurf = (skelSurface_t *)((byte *)newHeader + newHeader->ofsSurfaces);

    for (i = 0; i < pheader->numSurfaces; i++) {
        skeletorVertex_t *newVerts;
        skelVertex_t     *oldVerts;
        int               numVerts;
#if defined(__PS3__)
        if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKB convert-surface-enter path=%s surface=%d/%d", path, i + 1, pheader->numSurfaces);
#endif

        memcpy(newSurf, oldSurf, oldSurf->ofsVerts);
        if (newSurf->ofsCollapse > LongNoSwapPtr(&newSurf->ofsVerts)) {
            int newOffset =
                LongNoSwapPtr(&newSurf->ofsCollapse) + sizeof(unsigned int) * LongNoSwapPtr(&newSurf->numVerts);
            memcpy(&newSurf->ofsCollapse, &newOffset, sizeof(newSurf->ofsCollapse));
        }
        if (newSurf->ofsCollapseIndex > LongNoSwapPtr(&newSurf->ofsVerts)) {
            int newOffset =
                LongNoSwapPtr(&newSurf->ofsCollapseIndex) + sizeof(unsigned int) * LongNoSwapPtr(&newSurf->numVerts);
            memcpy(&newSurf->ofsCollapseIndex, &newOffset, sizeof(newSurf->ofsCollapseIndex));
        }
        if (newSurf->ofsTriangles > LongNoSwapPtr(&newSurf->ofsVerts)) {
            int newOffset =
                LongNoSwapPtr(&newSurf->ofsTriangles) + sizeof(unsigned int) * LongNoSwapPtr(&newSurf->numVerts);
            memcpy(&newSurf->ofsTriangles, &newOffset, sizeof(newSurf->ofsTriangles));
        }
        if (newSurf->ofsEnd > LongNoSwapPtr(&newSurf->ofsVerts)) {
            int newOffset = LongNoSwapPtr(&newSurf->ofsEnd) + sizeof(unsigned int) * LongNoSwapPtr(&newSurf->numVerts);
            memcpy(&newSurf->ofsEnd, &newOffset, sizeof(newSurf->ofsEnd));
        }

        oldVerts = (skelVertex_t *)((byte *)oldSurf + oldSurf->ofsVerts);
        newVerts = (skeletorVertex_t *)((byte *)newSurf + newSurf->ofsVerts);

        numVerts = LongNoSwapPtr(&oldSurf->numVerts);
        for (j = 0; j < numVerts; j++) {
            int numMorphs, numWeights;

            memcpy(newVerts->normal, oldVerts->normal, sizeof(newVerts->normal));
            memcpy(newVerts->texCoords, oldVerts->texCoords, sizeof(newVerts->texCoords));

            numMorphs  = 0;
            numWeights = LongNoSwapPtr(&oldVerts->numWeights);
            memcpy(&newVerts->numMorphs, &numMorphs, sizeof(newVerts->numMorphs));
            memcpy(&newVerts->numWeights, &numWeights, sizeof(newVerts->numWeights));

            skelWeight_t *newWeights = (skelWeight_t *)((byte *)newVerts + sizeof(skeletorVertex_t));

            for (k = 0; k < numWeights; k++) {
                memcpy(newWeights, &oldVerts->weights[k], sizeof(skelWeight_t));
                newWeights++;
            }

            oldVerts = (skelVertex_t *)((byte *)oldVerts + sizeof(skelWeight_t) * numWeights
                                        + (sizeof(skelVertex_t) - sizeof(skelWeight_t)));
            newVerts =
                (skeletorVertex_t *)((byte *)newVerts + sizeof(skeletorVertex_t) + sizeof(skelWeight_t) * numWeights);
        }

        memcpy(newVerts, oldVerts, LongNoSwapPtr(&oldSurf->ofsEnd) - ((byte *)oldVerts - (byte *)oldSurf));
        oldSurf = (skelSurface_t *)((byte *)oldSurf + LongNoSwapPtr(&oldSurf->ofsEnd));
        newSurf = (skelSurface_t *)((byte *)newSurf + LongNoSwapPtr(&newSurf->ofsEnd));
#if defined(__PS3__)
        if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKB convert-surface-exit path=%s surface=%d/%d", path, i + 1, pheader->numSurfaces);
#endif
    }

    memcpy(newSurf, oldSurf, pheader->ofsEnd - ((char *)oldSurf - (char *)pheader));
    TIKI_Free(pheader);
#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKB before-cache path=%s", path);
#endif
    TIKI_CacheFileSkel(newHeader, cache, length);
#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKB after-cache path=%s skel=%p", path, (void *)cache->skel);
#endif
    TIKI_Free(newHeader);
    cache_numskel++;
#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKB exit path=%s", path);
#endif

    return qtrue;
}

/*
===============
SaveLODFile
===============
*/
void SaveLODFile(const char *path, lodControl_t *LOD)
{
    fileHandle_t file = FS_FOpenFileWrite_HomeData(path);
    if (!file) {
        TIKI_Warning("SaveLODFile: Failed to open file %s\n", path);
        return;
    }

    FS_Write(LOD, sizeof(lodControl_t), file);
}

/*
===============
GetLODFile
===============
*/
void GetLODFile(skelcache_t *cache)
{
    lodControl_t      *LOD;
    char               pathLOD[256];
    char              *buf;
    int                i;
    skelSurfaceGame_t *pSurf;
    bool               bCanLod = false;
    char              *ext;
    int                length;

    pSurf = cache->skel->pSurfaces;

    for (i = 0; i < cache->skel->numSurfaces; i++) {
        if (pSurf->pCollapseIndex[0] != pSurf->pCollapseIndex[pSurf->numVerts - 1]) {
            bCanLod = true;
            break;
        }

        pSurf = pSurf->pNext;
    }

    if (!bCanLod) {
        return;
    }

    Q_strncpyz(pathLOD, cache->path, sizeof(pathLOD));
    ext = strstr(pathLOD, "skd");
    strcpy(ext, "lod");

    length = TIKI_ReadFileEx(pathLOD, (void **)&buf, true);
    if (length >= 0) {
        LOD = (lodControl_t *)TIKI_Alloc(sizeof(lodControl_t));
        memcpy(LOD, buf, length);
        TIKI_FreeFile(buf);

        LOD->minMetric = LittleFloat(LOD->minMetric);
        LOD->maxMetric = LittleFloat(LOD->maxMetric);

        for (i = 0; i < MAX_LOD_CURVE_POINTS; i++) {
            LOD->curve[i].pos = LittleFloat(LOD->curve[i].pos);
            LOD->curve[i].val = LittleFloat(LOD->curve[i].val);
        }

        for (i = 0; i < MAX_LOD_CURVE_CONSTS; i++) {
            LOD->consts[i].base   = LittleFloat(LOD->consts[i].base);
            LOD->consts[i].scale  = LittleFloat(LOD->consts[i].scale);
            LOD->consts[i].cutoff = LittleFloat(LOD->consts[i].cutoff);
        }
    } else {
        LOD               = (lodControl_t *)TIKI_Alloc(sizeof(lodControl_t));
        LOD->minMetric    = 1.0f;
        LOD->maxMetric    = 0.2f;
        LOD->curve[0].pos = 0.0f;
        LOD->curve[0].val = 0.0f;
        LOD->curve[1].pos = 0.5f;
        LOD->curve[1].val = (float)cache->skel->lodIndex[1];
        LOD->curve[2].pos = 0.8f;
        LOD->curve[2].val = (float)cache->skel->lodIndex[1];
        LOD->curve[3].pos = 0.95f;
        LOD->curve[3].val = (float)cache->skel->lodIndex[1];
        LOD->curve[4].pos = 1.0f;

        for (i = TIKI_SKEL_LOD_INDEXES; cache->skel->lodIndex[i] > cache->skel->lodIndex[3]; i--) {
            if (i <= 2) {
                break;
            }
        }

        LOD->curve[4].val = (float)cache->skel->lodIndex[i];

        for (i = 1; i < MAX_LOD_CURVE_POINTS - 1; i++) {
            if (LOD->curve[i].pos < LOD->curve[i - 1].pos) {
                LOD->curve[i].val = LOD->curve[i - 1].pos;
            }
        }
    }

    TIKI_CalcLodConsts(LOD);
    cache->skel->pLOD = LOD;
}

/*
===============
TIKI_CalcLodConsts
===============
*/
void TIKI_CalcLodConsts(lodControl_t *LOD)
{
    int i;

    for (i = 0; i < 4; i++) {
        float common =
            (LOD->curve[i + 1].val - LOD->curve[i + 0].val) / (LOD->curve[i + 1].pos - LOD->curve[i + 0].pos);

        LOD->consts[i].base = LOD->curve[i + 0].val
                            + (LOD->minMetric / (LOD->minMetric - LOD->maxMetric) - LOD->curve[i + 0].pos) * common;
        LOD->consts[i].scale  = common / (LOD->maxMetric - LOD->minMetric);
        LOD->consts[i].cutoff = LOD->minMetric + (LOD->maxMetric - LOD->minMetric) * LOD->curve[i + 0].pos;
    }
}

/*
===============
TIKI_LoadSKD
===============
*/
qboolean TIKI_LoadSKD(const char *path, skelcache_t *cache)
{
#if defined(__PS3__)
    const qboolean ps3TraceAmmobox = path && (strstr(path, "item_grenade_ammobox") != NULL || strstr(path, "grenade_box") != NULL);
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKD enter path=%s", path);
#endif
    int            i;
    skelHeader_t  *pheader;
    skelSurface_t *surf;
    int            version;
    unsigned int   header;
    int            length;

#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKD before-read path=%s", path);
#endif
    length = TIKI_ReadFileEx(path, (void **)&pheader, qtrue);
#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKD after-read path=%s length=%d buffer=%p", path, length, (void *)pheader);
#endif
    if (length < 0) {
        TIKI_DPrintf("Tiki:LoadAnim Couldn't load %s\n", path);
        return qfalse;
    }

    // The SKD must have at least one surface
    if (LittleLong(pheader->numSurfaces) <= 0) {
        TIKI_Error("^~^~^ TIKI_LoadSKD: %s has no surfaces\n", path);
        TIKI_FreeFile(pheader);
        return qfalse;
    }

    memset(cache, 0, sizeof(skelcache_t));
    strncpy(cache->path, path, sizeof(cache->path));

    // Check the signature
    header = pheader->ident;
    if (header != TIKI_SKD_HEADER_IDENT) {
        TIKI_Error("TIKI_LoadSKD: Tried to load '%s' as a skeletal base frame (File has invalid header)\n", path);
        TIKI_FreeFile(pheader);
        return qfalse;
    }

    // Check the version
    version = LittleLong(pheader->version);
    if (version != TIKI_SKD_HEADER_OLD_VERSION && version != TIKI_SKD_HEADER_VERSION) {
        TIKI_Error("TIKI_LoadSKD: %s has wrong version (%i should be %i)\n", path, version, TIKI_SKD_HEADER_VERSION);
        TIKI_FreeFile(pheader);
        return qfalse;
    }

#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKD header path=%s version=%d surfaces=%d bones=%d bytes=%d", path, version, LittleLong(pheader->numSurfaces), LittleLong(pheader->numBones), length);
#endif
    TIKI_SwapSkel(pheader);
#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKD after-swap path=%s surfaces=%d bones=%d", path, pheader->numSurfaces, pheader->numBones);
#endif

    surf = (skelSurface_t *)((byte *)pheader + pheader->ofsSurfaces);

    for (i = 0; i < pheader->numSurfaces; i++) {
        if (surf->numVerts > TIKI_MAX_VERTEXES) {
            TIKI_Error(
                "TIKI_LoadSKD: %s has more than %i verts on a surface (%i)", path, TIKI_MAX_VERTEXES, surf->numVerts
            );
            TIKI_FreeFile(pheader);
            return qfalse;
        }

        if (surf->numTriangles > TIKI_MAX_TRIANGLES) {
            TIKI_Error(
                "TIKI_LoadSKD: %s has more than %i triangles on a surface (%i)",
                path,
                TIKI_MAX_TRIANGLES,
                surf->numTriangles
            );
            TIKI_FreeFile(pheader);
            return qfalse;
        }

        surf = (skelSurface_t *)((byte *)surf + surf->ofsEnd);
    }

    if (pheader->numBones > TIKI_MAX_BONES) {
        TIKI_FreeFile(pheader);
        TIKI_Error("TIKI_LoadSKD: %s has more than %i bones (%i)\n", path, TIKI_MAX_BONES, pheader->numBones);
        return qfalse;
    }

    // Cache the skeleton
#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKD before-cache path=%s", path);
#endif
    TIKI_CacheFileSkel(pheader, cache, length);
    TIKI_FreeFile(pheader);
    cache_numskel++;
#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKD after-cache path=%s skel=%p", path, (void *)cache->skel);
#endif

    // Get the lod file associated with the skeleton
#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKD before-lod path=%s", path);
#endif
    GetLODFile(cache);
#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKD after-lod path=%s", path);
#endif
    TIKI_SortLOD(cache->skel);
#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKD exit path=%s", path);
#endif

    return true;
}

/*
===============
TIKI_GetSkel
===============
*/
skelHeaderGame_t *TIKI_GetSkel(int index)
{
    return skelcache[index].skel;
}

/*
===============
TIKI_GetSkelCache
===============
*/
int TIKI_GetSkelCache(skelHeaderGame_t *pSkel)
{
    for (int index = 0; index < cache_maxskel; index++) {
        if (skelcache[index].skel == pSkel) {
            return index;
        }
    }

    return -1;
}

/*
===============
TIKI_FreeSkel
===============
*/
void TIKI_FreeSkel(int index)
{
    if (index < 0 || index > cache_maxskel) {
        TIKI_Error("TIKI_FreeSkel: handle %d out of range\n", index);
        return;
    }

    if (!skelcache[index].skel) {
        TIKI_Error("TIKI_FreeSkel: NULL pointer for %s\n", skelcache[index].path);
        return;
    }

    TIKI_FreeSkelCache(&skelcache[index]);
    cache_numskel--;
}

/*
===============
TIKI_FreeSkelCache
===============
*/
void TIKI_FreeSkelCache(skelcache_t *cache)
{
    skelSurfaceGame_t *pSurf;

    if (cache->skel == NULL) {
        return;
    }

    for (pSurf = cache->skel->pSurfaces; pSurf != NULL; pSurf = pSurf->pNext) {
        if (pSurf->pStaticXyz) {
            TIKI_Free(pSurf->pStaticXyz);
        }
    }

    if (cache->skel->pLOD) {
        TIKI_Free(cache->skel->pLOD);
    }

    TIKI_Free(cache->skel);
    cache->skel    = NULL;
    cache->size    = 0;
    cache->path[0] = 0;
    cache->numuses = 0;
}

/*
===============
TIKI_FindSkel
===============
*/
skelcache_t *TIKI_FindSkel(const char *path)
{
    int          i;
    int          num;
    skelcache_t *cache;

    num = cache_numskel;

    for (i = 0; i < TIKI_MAX_SKELCACHE; i++) {
        cache = &skelcache[i];

        if (!cache->skel) {
            continue;
        }

        if (!strcmp(path, cache->path)) {
            return cache;
        }

        if (num == 0) {
            break;
        }
        num--;
    }

    return NULL;
}

/*
===============
TIKI_FindSkelByHeader

Added in OPM
Return the skelcache associated with the specified skelmodel
===============
*/
skelcache_t *TIKI_FindSkelByHeader(skelHeaderGame_t *skelmodel)
{
    int          i;
    int          num;
    skelcache_t *cache;

    num = cache_numskel;

    for (i = 0; i < TIKI_MAX_SKELCACHE; i++) {
        cache = &skelcache[i];

        if (!cache->skel) {
            continue;
        }

        if (cache->skel == skelmodel) {
            return cache;
        }

        if (num == 0) {
            break;
        }
        num--;
    }

    return NULL;
}

/*
===============
TIKI_FindFreeSkel
===============
*/
skelcache_t *TIKI_FindFreeSkel(void)
{
    int          i;
    skelcache_t *cache;

    for (i = 0; i < TIKI_MAX_SKELCACHE; i++) {
        cache = &skelcache[i];

        if (!cache->skel) {
            return cache;
        }
    }

    return NULL;
}

/*
===============
TIKI_RegisterSkel
===============
*/
int TIKI_RegisterSkel(const char *path, dtiki_t *tiki)
{
#if defined(__PS3__)
    const qboolean ps3TraceAmmobox = path && (strstr(path, "item_grenade_ammobox") != NULL || strstr(path, "grenade_box") != NULL);
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKEL register-enter path=%s", path);
#endif
    char         tempName[257];
    skelcache_t *cache;
    const char  *extension;

    // Find a skel with the same name
    cache = TIKI_FindSkel(path);
    if (cache) {
#if defined(__PS3__)
        if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKEL cache-hit path=%s index=%d", path, (int)(cache - skelcache));
#endif
        return cache - skelcache;
    }

    // Find a free skel
    cache = TIKI_FindFreeSkel();
    if (!cache) {
        TIKI_Error("TIKI_RegisterSkel: No free spots open in skel cache for %s\n", path);
        return -1;
    }
    
    cache->numuses = 0;

    if (cache - skelcache + 1 > cache_maxskel) {
        cache_maxskel = cache - skelcache + 1;
    }

    extension = TIKI_FileExtension(path);
#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKEL before-load path=%s ext=%s index=%d", path, extension, (int)(cache - skelcache));
#endif
    if (!strcmp(extension, "skb")) {
        if (!TIKI_LoadSKB(path, cache)) {
#if defined(__PS3__)
            if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKEL load-failed path=%s", path);
#endif
            return -1;
        }
    } else if (!strcmp(extension, "skd")) {
        if (!TIKI_LoadSKD(path, cache)) {
#if defined(__PS3__)
            if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKEL load-failed path=%s", path);
#endif
            return -1;
        }
#if defined(__PS3__)
        if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKEL after-load path=%s surfaces=%d bones=%d", path, cache->skel ? cache->skel->numSurfaces : -1, cache->skel ? cache->skel->numBones : -1);
#endif
    } else {
        TIKI_Error("TIKI_RegisterSkel: Unknown extension %s\n", extension);
        return -1;
    }

    Com_sprintf(tempName, sizeof(tempName), "l%s", path);
#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKEL before-ui-resource path=%s resource=%s", path, tempName);
#endif
    UI_LoadResource(tempName);
#if defined(__PS3__)
    if (ps3TraceAmmobox) PS3_BootTraceFastf("TIKI_SKEL register-exit path=%s index=%d", path, (int)(cache - skelcache));
#endif

    return cache - skelcache;
}
