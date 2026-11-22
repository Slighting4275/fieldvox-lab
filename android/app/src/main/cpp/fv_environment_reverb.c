/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "fv_environment_reverb.h"
#include "third_party/lvm/Reverb/lib/LVREV.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct FvEnvironmentReverb {
    LVREV_Handle_t handle;
    LVREV_MemoryTable_st memory;
    float delay[2400], gain;
    size_t position;
};
FvEnvironmentReverb *fv_environment_reverb_create(void) {
    FvEnvironmentReverb *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    LVREV_InstanceParams_st instance = {.MaxBlockSize=64, .SourceFormat=LVM_STEREO, .NumDelays=LVREV_DELAYLINES_4};
    if (LVREV_GetMemoryTable(NULL, &r->memory, &instance) != LVREV_SUCCESS) goto fail;
    for (unsigned i=0; i<LVREV_NR_MEMORY_REGIONS; i++) {
        r->memory.Region[i].pBaseAddress = calloc(1, r->memory.Region[i].Size ? r->memory.Region[i].Size : 1u);
        if (!r->memory.Region[i].pBaseAddress) goto fail;
    }
    if (LVREV_GetInstanceHandle(&r->handle, &r->memory, &instance) != LVREV_SUCCESS) goto fail;
    fv_environment_reverb_set_mix(r, 0.0f);
    return r;
fail:
    fv_environment_reverb_destroy(r); return NULL;
}
void fv_environment_reverb_destroy(FvEnvironmentReverb *r) {
    if (!r) return;
    for (unsigned i=0; i<LVREV_NR_MEMORY_REGIONS; i++) free(r->memory.Region[i].pBaseAddress);
    free(r);
}
void fv_environment_reverb_reset(FvEnvironmentReverb *r) {
    if (!r) return;
    LVREV_ClearAudioBuffers(r->handle);
    memset(r->delay, 0, sizeof(r->delay)); r->position=0;
}
void fv_environment_reverb_set_mix(FvEnvironmentReverb *r, float mix) {
    if (!r) return;
    mix = isfinite(mix) ? fmaxf(0.0f, fminf(1.0f, mix)) : 0.0f;
    /* Android LVREV maps diffusion to Density and density to RoomSize.
     * This C implementation's supported decay limit is seven seconds. */
    LVREV_ControlParams_st control = {.OperatingMode=LVM_MODE_ON, .SampleRate=LVM_FS_48000,
        .SourceFormat=LVM_MONO, .Level=100, .LPF=23999, .HPF=50,
        .T60=(LVM_UINT16)fmaxf(100.0f, fminf((float)LVREV_MAX_T60, mix*9000.0f)),
        .Density=(LVM_UINT16)(mix*100.0f), .Damping=50, .RoomSize=100};
    LVREV_SetControlParameters(r->handle, &control);
    r->gain = mix > 0.0f ? powf(10.0f, (mix*10000.0f-11000.0f)/2000.0f) : 0.0f;
}
void fv_environment_reverb_process(FvEnvironmentReverb *r, const float *send, float *wet, size_t frames) {
    if (!r || !send || !wet) return;
    for (size_t off=0; off<frames;) {
        const size_t count = frames-off > 64u ? 64u : frames-off;
        float input[64]={0}, stereo[128]={0};
        for (size_t i=0; i<count; i++) {
            input[i]=r->delay[r->position]; r->delay[r->position]=send[off+i]/32768.0f;
            r->position=(r->position+1u)%2400u;
        }
        const LVREV_ReturnStatus_en status=LVREV_Process(r->handle, input, stereo, (LVM_UINT16)count);
        for (size_t i=0; i<count; i++) wet[off+i]=status==LVREV_SUCCESS ?
            (stereo[i*2u]+stereo[i*2u+1u])*0.5f*32768.0f*r->gain : 0.0f;
        off+=count;
    }
}
