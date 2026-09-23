// SpeedTree branch fog pass. ps_3_0 port of stock STB1009.pso (ps_1_3 "mov r0, v0"), so the
// vs_3_0 STB1009.vso override never pairs with a ps_1_x shader.

float4 main(float4 color_0 : COLOR0) : COLOR0 {
    return color_0;
};
