#include <string>
namespace slg { namespace ocl {
std::string KernelSource_materialdefs_funcs_heterogeneousvol = 
"#line 2 \"materialdefs_funcs_heterogenousvol.cl\"\n"
"\n"
"/***************************************************************************\n"
" * Copyright 1998-2015 by authors (see AUTHORS.txt)                        *\n"
" *                                                                         *\n"
" *   This file is part of LuxRender.                                       *\n"
" *                                                                         *\n"
" * Licensed under the Apache License, Version 2.0 (the \"License\");         *\n"
" * you may not use this file except in compliance with the License.        *\n"
" * You may obtain a copy of the License at                                 *\n"
" *                                                                         *\n"
" *     http://www.apache.org/licenses/LICENSE-2.0                          *\n"
" *                                                                         *\n"
" * Unless required by applicable law or agreed to in writing, software     *\n"
" * distributed under the License is distributed on an \"AS IS\" BASIS,       *\n"
" * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*\n"
" * See the License for the specific language governing permissions and     *\n"
" * limitations under the License.                                          *\n"
" ***************************************************************************/\n"
"\n"
"#if defined(PARAM_HAS_VOLUMES)\n"
"float3 SchlickScatter_Evaluate(\n"
"		__global HitPoint *hitPoint, const float3 localEyeDir, const float3 localLightDir,\n"
"		BSDFEvent *event, float *directPdfW,\n"
"		const float3 sigmaS, const float3 sigmaA, const float3 g) {\n"
"	float3 r = sigmaS;\n"
"	if (r.x > 0.f)\n"
"		r.x /= r.x + sigmaA.x;\n"
"	else\n"
"		r.x = 1.f;\n"
"	if (r.y > 0.f)\n"
"		r.y /= r.y + sigmaA.y;\n"
"	else\n"
"		r.y = 1.f;\n"
"	if (r.z > 0.f)\n"
"		r.z /= r.z + sigmaA.z;\n"
"	else\n"
"		r.z = 1.f;\n"
"\n"
"	const float3 gValue = clamp(g, -1.f, 1.f);\n"
"	const float3 k = gValue * (1.55f - .55f * gValue * gValue);\n"
"\n"
"	*event = DIFFUSE | REFLECT;\n"
"\n"
"	const float dotEyeLight = dot(localEyeDir, localLightDir);\n"
"	const float kFilter = Spectrum_Filter(k);\n"
"	// 1+k*cos instead of 1-k*cos because localEyeDir is reversed compared to the\n"
"	// standard phase function definition\n"
"	const float compcostFilter = 1.f + kFilter * dotEyeLight;\n"
"	const float pdf = (1.f - kFilter * kFilter) / (compcostFilter * compcostFilter * (4.f * M_PI_F));\n"
"\n"
"	if (directPdfW)\n"
"		*directPdfW = pdf;\n"
"\n"
"	// 1+k*cos instead of 1-k*cos because localEyeDir is reversed compared to the\n"
"	// standard phase function definition\n"
"	const float3 compcostValue = 1.f + k * dotEyeLight;\n"
"\n"
"	return r * (1.f - k * k) / (compcostValue * compcostValue * (4.f * M_PI_F));\n"
"}\n"
"\n"
"float3 SchlickScatter_Sample(\n"
"		__global HitPoint *hitPoint, const float3 fixedDir, float3 *sampledDir,\n"
"		const float u0, const float u1, \n"
"#if defined(PARAM_HAS_PASSTHROUGH)\n"
"		const float passThroughEvent,\n"
"#endif\n"
"		float *pdfW, float *cosSampledDir, BSDFEvent *event,\n"
"		const BSDFEvent requestedEvent,\n"
"		const float3 sigmaS, const float3 sigmaA, const float3 g) {\n"
"	if (!(requestedEvent & (DIFFUSE | REFLECT)))\n"
"		return BLACK;\n"
"\n"
"	const float3 gValue = clamp(g, -1.f, 1.f);\n"
"	const float3 k = gValue * (1.55f - .55f * gValue * gValue);\n"
"	const float gFilter = Spectrum_Filter(k);\n"
"\n"
"	// Add a - because localEyeDir is reversed compared to the standard phase\n"
"	// function definition\n"
"	const float cost = -(2.f * u0 + gFilter - 1.f) / (2.f * gFilter * u0 - gFilter + 1.f);\n"
"\n"
"	float3 x, y;\n"
"	CoordinateSystem(fixedDir, &x, &y);\n"
"	*sampledDir = SphericalDirectionWithFrame(sqrt(fmax(0.f, 1.f - cost * cost)), cost,\n"
"			2.f * M_PI_F * u1, x, y, fixedDir);\n"
"\n"
"	// The - becomes a + because cost has been reversed above\n"
"	const float compcost = 1.f + gFilter * cost;\n"
"	*pdfW = (1.f - gFilter * gFilter) / (compcost * compcost * (4.f * M_PI_F));\n"
"	if (*pdfW <= 0.f)\n"
"		return BLACK;\n"
"\n"
"	*cosSampledDir = fabs((*sampledDir).z);\n"
"	*event = DIFFUSE | REFLECT;\n"
"\n"
"	float3 r = sigmaS;\n"
"	if (r.x > 0.f)\n"
"		r.x /= r.x + sigmaA.x;\n"
"	else\n"
"		r.x = 1.f;\n"
"	if (r.y > 0.f)\n"
"		r.y /= r.y + sigmaA.y;\n"
"	else\n"
"		r.y = 1.f;\n"
"	if (r.z > 0.f)\n"
"		r.z /= r.z + sigmaA.z;\n"
"	else\n"
"		r.z = 1.f;\n"
"\n"
"	return r;\n"
"}\n"
"#endif\n"
"\n"
"//------------------------------------------------------------------------------\n"
"// HeterogeneousVol material\n"
"//------------------------------------------------------------------------------\n"
"\n"
"#if defined (PARAM_ENABLE_MAT_HETEROGENEOUS_VOL)\n"
"\n"
"BSDFEvent HeterogeneousVolMaterial_GetEventTypes() {\n"
"	return DIFFUSE | REFLECT;\n"
"}\n"
"\n"
"float3 HeterogeneousVolMaterial_Evaluate(\n"
"		__global HitPoint *hitPoint, const float3 lightDir, const float3 eyeDir,\n"
"		BSDFEvent *event, float *directPdfW,\n"
"		const float3 sigmaSTexVal, const float3 sigmaATexVal, const float3 gTexVal) {\n"
"	return SchlickScatter_Evaluate(\n"
"			hitPoint, eyeDir, lightDir,\n"
"			event, directPdfW,\n"
"			clamp(sigmaSTexVal, 0.f, INFINITY), clamp(sigmaATexVal, 0.f, INFINITY), gTexVal);\n"
"}\n"
"\n"
"float3 HeterogeneousVolMaterial_Sample(\n"
"		__global HitPoint *hitPoint, const float3 fixedDir, float3 *sampledDir,\n"
"		const float u0, const float u1, \n"
"#if defined(PARAM_HAS_PASSTHROUGH)\n"
"		const float passThroughEvent,\n"
"#endif\n"
"		float *pdfW, float *cosSampledDir, BSDFEvent *event,\n"
"		const BSDFEvent requestedEvent,\n"
"		const float3 sigmaSTexVal, const float3 sigmaATexVal, const float3 gTexVal) {\n"
"	return SchlickScatter_Sample(\n"
"			hitPoint, fixedDir, sampledDir,\n"
"			u0, u1, \n"
"#if defined(PARAM_HAS_PASSTHROUGH)\n"
"			passThroughEvent,\n"
"#endif\n"
"			pdfW, cosSampledDir, event,\n"
"			requestedEvent,\n"
"			clamp(sigmaSTexVal, 0.f, INFINITY), clamp(sigmaATexVal, 0.f, INFINITY), gTexVal);\n"
"}\n"
"\n"
"#endif\n"
; } }
