#version 100
precision highp float;
uniform sampler2D Texture;
varying vec2 v_texCoord;
varying vec2 maskpos;
uniform mediump float warpX;
uniform mediump float warpY;
uniform mediump float expandX;
uniform mediump float expandY;
uniform mediump float brightboost;
uniform mediump float noiseIntensity;
uniform mediump float vignette;
uniform mediump float desaturate;
uniform mediump float desaturateEdges;
uniform mediump float u_Time;
void main ()
{
  lowp vec4 tmpvar_18_1;
  float tmpvar_17_2;
  mediump float tmpvar_13_3;
  mediump float tmpvar_12_4;
  mediump vec2 tmpvar_11_5;
  mediump vec3 pCol_10_6;
  mediump float tmpvar_9_7;
  mediump float x_8_8;
  lowp vec3 colour_7_9;
  mediump vec2 tmpvar_5_10;
  mediump vec2 tmpvar_4_11;
  mediump vec2 pos_3_12;
  mediump vec2 tmpvar_2_13;
  lowp vec3 pCol_1_14;
  tmpvar_4_11.x = expandX;
  tmpvar_4_11.y = expandY;
  pos_3_12 = (((v_texCoord - 0.5) / tmpvar_4_11) + 0.5);
  pos_3_12 = ((pos_3_12 * 2.0) - 1.0);
  tmpvar_5_10.x = (1.0 + ((pos_3_12.y * pos_3_12.y) * warpX));
  tmpvar_5_10.y = (1.0 + ((pos_3_12.x * pos_3_12.x) * warpY));
  pos_3_12 = (pos_3_12 * tmpvar_5_10);
  tmpvar_2_13 = ((pos_3_12 * 0.5) + 0.5);
  lowp vec4 tmpvar_15;
  tmpvar_15 = texture2D (Texture, tmpvar_2_13);
  x_8_8 = ((float(mod ((
    dot ((tmpvar_2_13 + vec2(u_Time)), vec2(12.9898, 78.233))
   + 3.141593), 6.283185))) - 3.141593);
  tmpvar_9_7 = (fract((
    ((1.273239 * x_8_8) - ((0.4052847 * x_8_8) * abs(x_8_8)))
   * 43758.55)) - 0.5);
  colour_7_9 = (tmpvar_15.xyz + (tmpvar_9_7 * noiseIntensity));
  tmpvar_11_5 = (tmpvar_2_13 - vec2(0.5, 0.5));
  tmpvar_12_4 = (dot (tmpvar_11_5, tmpvar_11_5) * 2.0);
  tmpvar_13_3 = (desaturate + (tmpvar_12_4 * desaturateEdges));
  pCol_10_6 = (((colour_7_9 + vec3(tmpvar_13_3)) / (1.0 + tmpvar_13_3)) * (1.0 - (tmpvar_12_4 * vignette)));
  pCol_1_14 = pCol_10_6;
  vec2 tmpvar_16;
  tmpvar_16 = floor(maskpos);
  float tmpvar_17;
  tmpvar_17 = fract((tmpvar_16.x / 3.0));
  float tmpvar_18;
  tmpvar_18 = fract((tmpvar_16.x / 6.0));
  tmpvar_17_2 = float((0.001 >= fract(
    (tmpvar_16.y / 2.0)
  )));
  pCol_1_14 = (pCol_1_14 * mix (vec3((
    mix (0.7498125, 1.333, float((tmpvar_17 >= 0.001)))
   * 
    mix (1.0, 0.75, (float((tmpvar_17 >= 0.001)) * ((tmpvar_17_2 * 
      (1.0 - float((tmpvar_18 >= 0.5)))
    ) + (
      (1.0 - tmpvar_17_2)
     * 
      float((tmpvar_18 >= 0.5))
    ))))
  )), vec3(1.0, 1.0, 1.0), (
    dot (tmpvar_15.xyz, vec3(0.299, 0.587, 0.114))
   * 0.9)));
  pCol_1_14 = (pCol_1_14 * brightboost);
  tmpvar_18_1.w = 1.0;
  tmpvar_18_1.xyz = pCol_1_14;
  gl_FragColor = tmpvar_18_1;
}
