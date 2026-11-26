#ifndef NRD_PORT_EYE_HLSLI
#define NRD_PORT_EYE_HLSLI
cbuffer NRDPortEyeCB : register(b3) {
 uint NRDPortEye; uint NRDPortOffsetX; uint NRDPortActiveWidth; uint NRDPortActiveHeight;
 uint NRDPortReplaceAmbient; float NRDPortILSaturation; float2 NRDPortPad;
};
#endif
