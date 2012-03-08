#include "pathocl/kernels/kernels.h"
std::string luxrays::KernelSource_PathOCL_kernel_filters = 
"/***************************************************************************\n"
" *   Copyright (C) 1998-2010 by authors (see AUTHORS.txt )                 *\n"
" *                                                                         *\n"
" *   This file is part of LuxRays.                                         *\n"
" *                                                                         *\n"
" *   LuxRays is free software; you can redistribute it and/or modify       *\n"
" *   it under the terms of the GNU General Public License as published by  *\n"
" *   the Free Software Foundation; either version 3 of the License, or     *\n"
" *   (at your option) any later version.                                   *\n"
" *                                                                         *\n"
" *   LuxRays is distributed in the hope that it will be useful,            *\n"
" *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *\n"
" *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *\n"
" *   GNU General Public License for more details.                          *\n"
" *                                                                         *\n"
" *   You should have received a copy of the GNU General Public License     *\n"
" *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *\n"
" *                                                                         *\n"
" *   LuxRays website: http://www.luxrender.net                             *\n"
" ***************************************************************************/\n"
"\n"
"// Line 451 + 268 = 719\n"
"\n"
"//------------------------------------------------------------------------------\n"
"// Pixel related functions\n"
"//------------------------------------------------------------------------------\n"
"\n"
"void PixelIndex2XY(const uint index, uint *x, uint *y) {\n"
"	*y = index / PARAM_IMAGE_WIDTH;\n"
"	*x = index - (*y) * PARAM_IMAGE_WIDTH;\n"
"}\n"
"\n"
"uint XY2PixelIndex(const uint x, const uint y) {\n"
"	return x + y * PARAM_IMAGE_WIDTH;\n"
"}\n"
"\n"
"uint XY2FrameBufferIndex(const int x, const int y) {\n"
"	return x + 1 + (y + 1) * (PARAM_IMAGE_WIDTH + 2);\n"
"}\n"
"\n"
"uint InitialPixelIndex(const size_t gid) {\n"
"	return gid % (PARAM_IMAGE_WIDTH * PARAM_IMAGE_HEIGHT);\n"
"}\n"
"\n"
"uint NextPixelIndex(const uint i) {\n"
"	return (i + PARAM_TASK_COUNT) % (PARAM_IMAGE_WIDTH * PARAM_IMAGE_HEIGHT);\n"
"}\n"
"\n"
"uint PixelIndexFloat(const float u) {\n"
"	const uint pixelCountPerTask = PARAM_IMAGE_WIDTH * PARAM_IMAGE_HEIGHT;\n"
"	const uint i = min((uint)floor(pixelCountPerTask * u), (uint)(pixelCountPerTask - 1));\n"
"\n"
"	return i;\n"
"}\n"
"\n"
"uint PixelIndexFloat2D(const float ux, const float uy) {\n"
"	const uint x = min((uint)floor(PARAM_IMAGE_WIDTH * ux), (uint)(PARAM_IMAGE_WIDTH - 1));\n"
"	const uint y = min((uint)floor(PARAM_IMAGE_HEIGHT * uy), (uint)(PARAM_IMAGE_HEIGHT - 1));\n"
"\n"
"	return XY2PixelIndex(x, y);\n"
"}\n"
"\n"
"uint PixelIndexFloat2DWithOffset(const float ux, const float uy, float *ox, float *oy) {\n"
"	const float px = PARAM_IMAGE_WIDTH * ux;\n"
"	const float py = PARAM_IMAGE_HEIGHT * uy;\n"
"\n"
"	const uint x = min((uint)floor(px), (uint)(PARAM_IMAGE_WIDTH - 1));\n"
"	const uint y = min((uint)floor(py), (uint)(PARAM_IMAGE_HEIGHT - 1));\n"
"\n"
"	*ox = px - (float)x;\n"
"	*oy = py - (float)y;\n"
"\n"
"	return XY2PixelIndex(x, y);\n"
"}\n"
"\n"
"bool IsValidPixelXY(const int x, const int y) {\n"
"	return (x >= 0) && (x < PARAM_IMAGE_WIDTH) && (y >= 0) && (y < PARAM_IMAGE_HEIGHT);\n"
"}\n"
"\n"
"//------------------------------------------------------------------------------\n"
"// Image filtering related functions\n"
"//------------------------------------------------------------------------------\n"
"\n"
"#if (PARAM_IMAGE_FILTER_TYPE == 0)\n"
"\n"
"// Nothing\n"
"\n"
"#elif (PARAM_IMAGE_FILTER_TYPE == 1)\n"
"\n"
"// Box Filter\n"
"float ImageFilter_Evaluate(const float x, const float y) {\n"
"	return 1.f;\n"
"}\n"
"\n"
"#elif (PARAM_IMAGE_FILTER_TYPE == 2)\n"
"\n"
"float Gaussian(const float d, const float expv) {\n"
"	return max(0.f, exp(-PARAM_IMAGE_FILTER_GAUSSIAN_ALPHA * d * d) - expv);\n"
"}\n"
"\n"
"// Gaussian Filter\n"
"float ImageFilter_Evaluate(const float x, const float y) {\n"
"	return Gaussian(x,\n"
"			exp(-PARAM_IMAGE_FILTER_GAUSSIAN_ALPHA * PARAM_IMAGE_FILTER_WIDTH_X * PARAM_IMAGE_FILTER_WIDTH_X)) *\n"
"		Gaussian(y, \n"
"			exp(-PARAM_IMAGE_FILTER_GAUSSIAN_ALPHA * PARAM_IMAGE_FILTER_WIDTH_Y * PARAM_IMAGE_FILTER_WIDTH_Y));\n"
"}\n"
"\n"
"#elif (PARAM_IMAGE_FILTER_TYPE == 3)\n"
"\n"
"float Mitchell1D(float x) {\n"
"	const float B = PARAM_IMAGE_FILTER_MITCHELL_B;\n"
"	const float C = PARAM_IMAGE_FILTER_MITCHELL_C;\n"
"\n"
"	if (x >= 1.f)\n"
"		return 0.f;\n"
"	x = fabs(2.f * x);\n"
"\n"
"	if (x > 1.f)\n"
"		return (((-B / 6.f - C) * x + (B + 5.f * C)) * x +\n"
"			(-2.f * B - 8.f * C)) * x + (4.f / 3.f * B + 4.f * C);\n"
"	else\n"
"		return ((2.f - 1.5f * B - C) * x +\n"
"			(-3.f + 2.f * B + C)) * x * x +\n"
"			(1.f - B / 3.f);\n"
"}\n"
"\n"
"// Mitchell Filter\n"
"float ImageFilter_Evaluate(const float x, const float y) {\n"
"	const float distance = native_sqrt(\n"
"			x * x * (1.f / (PARAM_IMAGE_FILTER_WIDTH_X * PARAM_IMAGE_FILTER_WIDTH_X)) +\n"
"			y * y * (1.f / (PARAM_IMAGE_FILTER_WIDTH_Y * PARAM_IMAGE_FILTER_WIDTH_Y)));\n"
"\n"
"	return Mitchell1D(distance);\n"
"}\n"
"\n"
"#else\n"
"\n"
"Error: unknown image filter !!!\n"
"\n"
"#endif\n"
"\n"
"void Pixel_AddRadiance(__global Pixel *pixel, Spectrum *rad, const float weight) {\n"
"	/*if (isnan(rad->r) || isinf(rad->r) ||\n"
"			isnan(rad->g) || isinf(rad->g) ||\n"
"			isnan(rad->b) || isinf(rad->b) ||\n"
"			isnan(weight) || isinf(weight))\n"
"		printf(\"(NaN/Inf. error: (%f, %f, %f) [%f]\\n\", rad->r, rad->g, rad->b, weight);*/\n"
"	\n"
"#if defined(__APPLE_FIX__)\n"
"\n"
"#if defined(PARAM_USE_PIXEL_ATOMICS)\n"
"	AtomicAdd(&pixel->c.r, weight * rad->r);\n"
"	AtomicAdd(&pixel->c.g, weight * rad->g);\n"
"	AtomicAdd(&pixel->c.b, weight * rad->b);\n"
"	AtomicAdd(&pixel->count, weight);\n"
"#else\n"
"	pixel->c.r += weight * rad->r;\n"
"	pixel->c.g += weight * rad->g;\n"
"	pixel->c.b += weight * rad->b;\n"
"	pixel->count += weight;\n"
"#endif\n"
"\n"
"#else\n"
"\n"
"	float4 s;\n"
"	s.x = rad->r;\n"
"	s.y = rad->g;\n"
"	s.z = rad->b;\n"
"	s.w = 1.f;\n"
"	s *= weight;\n"
"\n"
"#if defined(PARAM_USE_PIXEL_ATOMICS)\n"
"	AtomicAdd(&pixel->c.r, s.x);\n"
"	AtomicAdd(&pixel->c.g, s.y);\n"
"	AtomicAdd(&pixel->c.b, s.z);\n"
"	AtomicAdd(&pixel->count, s.w);\n"
"#else\n"
"	float4 p = vload4(0, (__global float *)pixel);\n"
"	p += s;\n"
"	vstore4(p, 0, (__global float *)pixel);\n"
"#endif\n"
"\n"
"#endif\n"
"}\n"
"\n"
"#if defined(PARAM_ENABLE_ALPHA_CHANNEL)\n"
"\n"
"void Pixel_AddAlpha(__global AlphaPixel *apixel, const float alpha, const float weight) {\n"
"#if defined(PARAM_USE_PIXEL_ATOMICS)\n"
"	AtomicAdd(&apixel->alpha, weight * alpha);\n"
"	AtomicAdd(&pixel->count, weight);\n"
"#else\n"
"	apixel->alpha += weight * alpha;\n"
"	apixel->count += weight;\n"
"#endif\n"
"}\n"
"#endif\n"
"\n"
"#if (PARAM_IMAGE_FILTER_TYPE == 1) || (PARAM_IMAGE_FILTER_TYPE == 2) || (PARAM_IMAGE_FILTER_TYPE == 3)\n"
"void Pixel_AddFilteredRadiance(__global Pixel *pixel, Spectrum *rad,\n"
"	const float distX, const float distY, const float weight) {\n"
"	const float filterWeight = ImageFilter_Evaluate(distX, distY);\n"
"\n"
"	Pixel_AddRadiance(pixel, rad, weight * filterWeight);\n"
"}\n"
"\n"
"#if defined(PARAM_ENABLE_ALPHA_CHANNEL)\n"
"void Pixel_AddFilteredAlpha(__global AlphaPixel *apixel, const float alpha,\n"
"	const float distX, const float distY, const float weight) {\n"
"	const float filterWeight = ImageFilter_Evaluate(distX, distY);\n"
"\n"
"	Pixel_AddAlpha(apixel, alpha, weight * filterWeight);\n"
"}\n"
"#endif\n"
"\n"
"#endif\n"
"\n"
"#if (PARAM_IMAGE_FILTER_TYPE == 0)\n"
"\n"
"void SplatSample(__global Pixel *frameBuffer,\n"
"#if defined(PARAM_ENABLE_ALPHA_CHANNEL)\n"
"		__global AlphaPixel *alphaFrameBuffer,\n"
"#endif\n"
"		const uint pixelIndex, Spectrum *radiance,\n"
"#if defined(PARAM_ENABLE_ALPHA_CHANNEL)\n"
"		const float alpha,\n"
"#endif\n"
"		const float weight) {\n"
"	uint ux, uy;\n"
"	PixelIndex2XY(pixelIndex, &ux, &uy);\n"
"	int x = (int)ux;\n"
"	int y = (int)uy;\n"
"	const uint pindex = XY2FrameBufferIndex(x, y);\n"
"	__global Pixel *pixel = &frameBuffer[pindex];\n"
"\n"
"	Pixel_AddRadiance(pixel, radiance, weight);\n"
"\n"
"#if defined(PARAM_ENABLE_ALPHA_CHANNEL)\n"
"	__global AlphaPixel *apixel = &alphaFrameBuffer[pindex];\n"
"	Pixel_AddAlpha(apixel, alpha, weight);\n"
"#endif\n"
"}\n"
"\n"
"#elif (PARAM_IMAGE_FILTER_TYPE == 1) || (PARAM_IMAGE_FILTER_TYPE == 2) || (PARAM_IMAGE_FILTER_TYPE == 3)\n"
"\n"
"void SplatSample(__global Pixel *frameBuffer,\n"
"#if defined(PARAM_ENABLE_ALPHA_CHANNEL)\n"
"		__global AlphaPixel *alphaFrameBuffer,\n"
"#endif\n"
"		const uint pixelIndex, const float sx, const float sy, Spectrum *radiance,\n"
"#if defined(PARAM_ENABLE_ALPHA_CHANNEL)\n"
"		const float alpha,\n"
"#endif\n"
"		const float weight) {\n"
"	uint ux, uy;\n"
"	PixelIndex2XY(pixelIndex, &ux, &uy);\n"
"	int x = (int)ux;\n"
"	int y = (int)uy;\n"
"\n"
"	{\n"
"		__global Pixel *pixel = &frameBuffer[XY2FrameBufferIndex(x - 1, y - 1)];\n"
"		Pixel_AddFilteredRadiance(pixel, radiance, sx + 1.f, sy + 1.f, weight);\n"
"		pixel = &frameBuffer[XY2FrameBufferIndex(x, y - 1)];\n"
"		Pixel_AddFilteredRadiance(pixel, radiance, sx, sy + 1.f, weight);\n"
"		pixel = &frameBuffer[XY2FrameBufferIndex(x + 1, y - 1)];\n"
"		Pixel_AddFilteredRadiance(pixel, radiance, sx - 1.f, sy + 1.f, weight);\n"
"\n"
"		pixel = &frameBuffer[XY2FrameBufferIndex(x - 1, y)];\n"
"		Pixel_AddFilteredRadiance(pixel, radiance, sx + 1.f, sy, weight);\n"
"		pixel = &frameBuffer[XY2FrameBufferIndex(x, y)];\n"
"		Pixel_AddFilteredRadiance(pixel, radiance, sx, sy, weight);\n"
"		pixel = &frameBuffer[XY2FrameBufferIndex(x + 1, y)];\n"
"		Pixel_AddFilteredRadiance(pixel, radiance, sx - 1.f, sy, weight);\n"
"\n"
"		pixel = &frameBuffer[XY2FrameBufferIndex(x - 1, y + 1)];\n"
"		Pixel_AddFilteredRadiance(pixel, radiance, sx + 1.f, sy - 1.f, weight);\n"
"		pixel = &frameBuffer[XY2FrameBufferIndex(x, y + 1)];\n"
"		Pixel_AddFilteredRadiance(pixel, radiance, sx, sy - 1.f, weight);\n"
"		pixel = &frameBuffer[XY2FrameBufferIndex(x + 1, y + 1)];\n"
"		Pixel_AddFilteredRadiance(pixel, radiance, sx - 1.f, sy - 1.f, weight);\n"
"	}\n"
"\n"
"#if defined(PARAM_ENABLE_ALPHA_CHANNEL)\n"
"	{\n"
"		__global AlphaPixel *apixel = &alphaFrameBuffer[XY2FrameBufferIndex(x - 1, y - 1)];\n"
"		Pixel_AddFilteredAlpha(apixel, alpha, sx + 1.f, sy + 1.f, weight);\n"
"		apixel = &alphaFrameBuffer[XY2FrameBufferIndex(x, y - 1)];\n"
"		Pixel_AddFilteredAlpha(apixel, alpha, sx, sy + 1.f, weight);\n"
"		apixel = &alphaFrameBuffer[XY2FrameBufferIndex(x + 1, y - 1)];\n"
"		Pixel_AddFilteredAlpha(apixel, alpha, sx - 1.f, sy + 1.f, weight);\n"
"\n"
"		apixel = &alphaFrameBuffer[XY2FrameBufferIndex(x - 1, y)];\n"
"		Pixel_AddFilteredAlpha(apixel, alpha, sx + 1.f, sy, weight);\n"
"		apixel = &alphaFrameBuffer[XY2FrameBufferIndex(x, y)];\n"
"		Pixel_AddFilteredAlpha(apixel, alpha, sx, sy, weight);\n"
"		apixel = &alphaFrameBuffer[XY2FrameBufferIndex(x + 1, y)];\n"
"		Pixel_AddFilteredAlpha(apixel, alpha, sx - 1.f, sy, weight);\n"
"\n"
"		apixel = &alphaFrameBuffer[XY2FrameBufferIndex(x - 1, y + 1)];\n"
"		Pixel_AddFilteredAlpha(apixel, alpha, sx + 1.f, sy - 1.f, weight);\n"
"		apixel = &alphaFrameBuffer[XY2FrameBufferIndex(x, y + 1)];\n"
"		Pixel_AddFilteredAlpha(apixel, alpha, sx, sy - 1.f, weight);\n"
"		apixel = &alphaFrameBuffer[XY2FrameBufferIndex(x + 1, y + 1)];\n"
"		Pixel_AddFilteredAlpha(apixel, alpha, sx - 1.f, sy - 1.f, weight);\n"
"	}\n"
"#endif\n"
"}\n"
"\n"
"#else\n"
"\n"
"Error: unknown image filter !!!\n"
"\n"
"#endif\n"
;
