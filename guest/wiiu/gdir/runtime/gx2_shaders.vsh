; The vertex shader: the scene and HUD vertices come already transformed
; (clip space) from the CPU, as the PS2 guest's EE sends them to the GS, so
; this only passes them on. Attributes (location -> R(location+1)):
; 0 position (float4), 1 colour (unorm8 x4), 2 fog (unorm8 x4: rgb the fog
; colour, a the surface left), 3 uv (float2).
; $MODE = "UniformRegister"
; $ATTRIB_VARS[0].NAME = "aPos"
; $ATTRIB_VARS[0].TYPE = "vec4"
; $ATTRIB_VARS[0].LOCATION = 0
; $ATTRIB_VARS[1].NAME = "aCol"
; $ATTRIB_VARS[1].TYPE = "vec4"
; $ATTRIB_VARS[1].LOCATION = 1
; $ATTRIB_VARS[2].NAME = "aFog"
; $ATTRIB_VARS[2].TYPE = "vec4"
; $ATTRIB_VARS[2].LOCATION = 2
; $ATTRIB_VARS[3].NAME = "aUv"
; $ATTRIB_VARS[3].TYPE = "vec2"
; $ATTRIB_VARS[3].LOCATION = 3
; $SQ_PGM_RESOURCES_VS.NUM_GPRS = 5
; $SQ_PGM_RESOURCES_VS.STACK_SIZE = 1
; $SPI_VS_OUT_CONFIG.VS_EXPORT_COUNT = 2
; $NUM_SPI_VS_OUT_ID = 1
; $SPI_VS_OUT_ID[0].SEMANTIC_0 = 0
; $SPI_VS_OUT_ID[0].SEMANTIC_1 = 1
; $SPI_VS_OUT_ID[0].SEMANTIC_2 = 2
; $SPI_VS_OUT_ID[0].SEMANTIC_3 = 255
; $SQ_VTX_SEMANTIC_CLEAR = 0xFFFFFFF0
; $NUM_SQ_VTX_SEMANTIC = 4
; $SQ_VTX_SEMANTIC[0].SEMANTIC_ID = 0
; $SQ_VTX_SEMANTIC[1].SEMANTIC_ID = 1
; $SQ_VTX_SEMANTIC[2].SEMANTIC_ID = 2
; $SQ_VTX_SEMANTIC[3].SEMANTIC_ID = 3
; $VGT_VERTEX_REUSE_BLOCK_CNTL.VTX_REUSE_DEPTH = 14
; $VGT_HOS_REUSE_DEPTH.REUSE_DEPTH = 16

00 CALL_FS NO_BARRIER
01 EXP_DONE: POS0, R1.xyzw
02 EXP: PARAM0, R2.xyzw NO_BARRIER
03 EXP: PARAM1, R3.xyzw NO_BARRIER
04 EXP_DONE: PARAM2, R4.xyzw NO_BARRIER
END_OF_PROGRAM
