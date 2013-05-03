/***************************************************************************
 *   Copyright (C) 1998-2013 by authors (see AUTHORS.txt)                  *
 *                                                                         *
 *   This file is part of LuxRays.                                         *
 *                                                                         *
 *   LuxRays is free software; you can redistribute it and/or modify       *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   LuxRays is distributed in the hope that it will be useful,            *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 *                                                                         *
 *   LuxRays website: http://www.luxrender.net                             *
 ***************************************************************************/

#if !defined(LUXRAYS_DISABLE_OPENCL)

#include <boost/lexical_cast.hpp>

#include "slg/slg.h"
#include "slg/engines/pathocl/pathocl.h"
#include "slg/engines/pathocl/rtpathocl.h"
#include "slg/kernels/kernels.h"
#include "slg/renderconfig.h"

#include "luxrays/core/geometry/transform.h"
#include "luxrays/accelerators/mqbvhaccel.h"
#include "luxrays/accelerators/bvhaccel.h"
#include "luxrays/utils/ocl.h"
#include "luxrays/core/oclintersectiondevice.h"
#include "luxrays/kernels/kernels.h"

using namespace std;
using namespace luxrays;
using namespace slg;

#define SOBOL_MAXDEPTH 6

//------------------------------------------------------------------------------
// PathOCLRenderThread
//------------------------------------------------------------------------------

PathOCLRenderThread::PathOCLRenderThread(const u_int index,
		OpenCLIntersectionDevice *device, PathOCLRenderEngine *re) {
	intersectionDevice = device;

	renderThread = NULL;

	threadIndex = index;
	renderEngine = re;
	started = false;
	editMode = false;
	frameBuffer = NULL;
	alphaFrameBuffer = NULL;
	gpuTaskStats = NULL;

	kernelsParameters = "";
	initKernel = NULL;
	initFBKernel = NULL;
	advancePathsKernel = NULL;

	clearFBKernel = NULL;
	clearSBKernel = NULL;
	mergeFBKernel = NULL;
	normalizeFBKernel = NULL;
	applyBlurFilterXR1Kernel = NULL;
	applyBlurFilterYR1Kernel = NULL;
	toneMapLinearKernel = NULL;
	updateScreenBufferKernel = NULL;

	raysBuff = NULL;
	hitsBuff = NULL;
	tasksBuff = NULL;
	sampleDataBuff = NULL;
	taskStatsBuff = NULL;
	frameBufferBuff = NULL;
	alphaFrameBufferBuff = NULL;
	materialsBuff = NULL;
	texturesBuff = NULL;
	meshIDBuff = NULL;
	meshDescsBuff = NULL;
	meshMatsBuff = NULL;
	infiniteLightBuff = NULL;
	sunLightBuff = NULL;
	skyLightBuff = NULL;
	vertsBuff = NULL;
	normalsBuff = NULL;
	uvsBuff = NULL;
	colsBuff = NULL;
	alphasBuff = NULL;
	trianglesBuff = NULL;
	cameraBuff = NULL;
	triLightDefsBuff = NULL;
	meshTriLightDefsOffsetBuff = NULL;
	imageMapDescsBuff = NULL;

	// Check the kind of kernel cache to use
	std::string type = re->renderConfig->cfg.GetString("opencl.kernelcache", "NONE");
	if (type == "PERSISTENT")
		kernelCache = new oclKernelPersistentCache("slg_" + string(SLG_VERSION_MAJOR) + "." + string(SLG_VERSION_MINOR));
	else if (type == "VOLATILE")
		kernelCache = new oclKernelVolatileCache();
	else if (type == "NONE")
		kernelCache = new oclKernelDummyCache();
	else
		throw std::runtime_error("Unknown opencl.kernelcache type: " + type);
}

PathOCLRenderThread::~PathOCLRenderThread() {
	if (editMode)
		EndEdit(EditActionList());
	if (started)
		Stop();

	delete initKernel;
	delete initFBKernel;
	delete advancePathsKernel;
	delete clearFBKernel;
	delete clearSBKernel;
	delete mergeFBKernel;
	delete normalizeFBKernel;
	delete applyBlurFilterXR1Kernel;
	delete applyBlurFilterYR1Kernel;
	delete toneMapLinearKernel;
	delete updateScreenBufferKernel;

	delete[] frameBuffer;
	delete[] alphaFrameBuffer;
	delete[] gpuTaskStats;

	delete kernelCache;
}

void PathOCLRenderThread::AllocOCLBufferRO(cl::Buffer **buff, void *src, const size_t size, const string &desc) {
	// Check if the buffer is too big
	if (intersectionDevice->GetDeviceDesc()->GetMaxMemoryAllocSize() < size) {
		stringstream ss;
		ss << "The " << desc << " buffer is too big for " << intersectionDevice->GetName() << 
				" device (i.e. CL_DEVICE_MAX_MEM_ALLOC_SIZE=" <<
				intersectionDevice->GetDeviceDesc()->GetMaxMemoryAllocSize() <<
				"): try to reduce related parameters";
		throw std::runtime_error(ss.str());
	}

	if (*buff) {
		// Check the size of the already allocated buffer

		if (size == (*buff)->getInfo<CL_MEM_SIZE>()) {
			// I can reuse the buffer; just update the content

			//SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] " << desc << " buffer updated for size: " << (size / 1024) << "Kbytes");
			cl::CommandQueue &oclQueue = intersectionDevice->GetOpenCLQueue();
			oclQueue.enqueueWriteBuffer(**buff, CL_FALSE, 0, size, src);
			return;
		} else {
			// Free the buffer
			intersectionDevice->FreeMemory((*buff)->getInfo<CL_MEM_SIZE>());
			delete *buff;
		}
	}

	cl::Context &oclContext = intersectionDevice->GetOpenCLContext();

	SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] " << desc << " buffer size: " <<
			(size < 10000 ? size : (size / 1024)) << (size < 10000 ? "bytes" : "Kbytes"));
	*buff = new cl::Buffer(oclContext,
			CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
			size, src);
	intersectionDevice->AllocMemory((*buff)->getInfo<CL_MEM_SIZE>());
}

void PathOCLRenderThread::AllocOCLBufferRW(cl::Buffer **buff, const size_t size, const string &desc) {
	// Check if the buffer is too big
	if (intersectionDevice->GetDeviceDesc()->GetMaxMemoryAllocSize() < size) {
		stringstream ss;
		ss << "The " << desc << " buffer is too big for " << intersectionDevice->GetName() << 
				" device (i.e. CL_DEVICE_MAX_MEM_ALLOC_SIZE=" <<
				intersectionDevice->GetDeviceDesc()->GetMaxMemoryAllocSize() <<
				"): try to reduce related parameters";
		throw std::runtime_error(ss.str());
	}

	if (*buff) {
		// Check the size of the already allocated buffer

		if (size == (*buff)->getInfo<CL_MEM_SIZE>()) {
			// I can reuse the buffer
			//SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] " << desc << " buffer reused for size: " << (size / 1024) << "Kbytes");
			return;
		} else {
			// Free the buffer
			intersectionDevice->FreeMemory((*buff)->getInfo<CL_MEM_SIZE>());
			delete *buff;
		}
	}

	cl::Context &oclContext = intersectionDevice->GetOpenCLContext();
 
	SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] " << desc << " buffer size: " <<
			(size < 10000 ? size : (size / 1024)) << (size < 10000 ? "bytes" : "Kbytes"));
	*buff = new cl::Buffer(oclContext,
			CL_MEM_READ_WRITE,
			size);
	intersectionDevice->AllocMemory((*buff)->getInfo<CL_MEM_SIZE>());
}

void PathOCLRenderThread::FreeOCLBuffer(cl::Buffer **buff) {
	if (*buff) {

		intersectionDevice->FreeMemory((*buff)->getInfo<CL_MEM_SIZE>());
		delete *buff;
		*buff = NULL;
	}
}

void PathOCLRenderThread::InitFrameBuffer() {
	//--------------------------------------------------------------------------
	// FrameBuffer definition
	//--------------------------------------------------------------------------

	frameBufferPixelCount =	(renderEngine->film->GetWidth() + 2) * (renderEngine->film->GetHeight() + 2);

	// Delete previous allocated frameBuffer
	delete[] frameBuffer;
	frameBuffer = new slg::ocl::Pixel[frameBufferPixelCount];

	for (u_int i = 0; i < frameBufferPixelCount; ++i) {
		frameBuffer[i].c.r = 0.f;
		frameBuffer[i].c.g = 0.f;
		frameBuffer[i].c.b = 0.f;
		frameBuffer[i].count = 0.f;
	}

	AllocOCLBufferRW(&frameBufferBuff, sizeof(slg::ocl::Pixel) * frameBufferPixelCount, "FrameBuffer");

	delete[] alphaFrameBuffer;
	alphaFrameBuffer = NULL;

	// Check if the film has an alpha channel
	if (renderEngine->film->IsAlphaChannelEnabled()) {
		alphaFrameBuffer = new slg::ocl::AlphaPixel[frameBufferPixelCount];

		for (u_int i = 0; i < frameBufferPixelCount; ++i)
			alphaFrameBuffer[i].alpha = 0.f;

		AllocOCLBufferRW(&alphaFrameBufferBuff, sizeof(slg::ocl::AlphaPixel) * frameBufferPixelCount, "Alpha Channel FrameBuffer");
	}
}

void PathOCLRenderThread::InitCamera() {
	AllocOCLBufferRO(&cameraBuff, &renderEngine->compiledScene->camera,
			sizeof(slg::ocl::Camera), "Camera");
}

void PathOCLRenderThread::InitGeometry() {
	Scene *scene = renderEngine->renderConfig->scene;
	CompiledScene *cscene = renderEngine->compiledScene;

	const u_int trianglesCount = scene->dataSet->GetTotalTriangleCount();
	AllocOCLBufferRO(&meshIDBuff, (void *)cscene->meshIDs,
			sizeof(u_int) * trianglesCount, "MeshIDs");

	if (cscene->normals.size() > 0)
		AllocOCLBufferRO(&normalsBuff, &cscene->normals[0],
				sizeof(Normal) * cscene->normals.size(), "Normals");
	else
		FreeOCLBuffer(&normalsBuff);

	if (cscene->uvs.size() > 0)
		AllocOCLBufferRO(&uvsBuff, &cscene->uvs[0],
			sizeof(UV) * cscene->uvs.size(), "UVs");
	else
		FreeOCLBuffer(&uvsBuff);

	if (cscene->cols.size() > 0)
		AllocOCLBufferRO(&colsBuff, &cscene->cols[0],
			sizeof(Spectrum) * cscene->cols.size(), "Colors");
	else
		FreeOCLBuffer(&colsBuff);

	if (cscene->alphas.size() > 0)
		AllocOCLBufferRO(&alphasBuff, &cscene->alphas[0],
			sizeof(float) * cscene->alphas.size(), "Alphas");
	else
		FreeOCLBuffer(&alphasBuff);

	AllocOCLBufferRO(&vertsBuff, &cscene->verts[0],
		sizeof(Point) * cscene->verts.size(), "Vertices");

	AllocOCLBufferRO(&trianglesBuff, &cscene->tris[0],
		sizeof(Triangle) * cscene->tris.size(), "Triangles");

	AllocOCLBufferRO(&meshDescsBuff, &cscene->meshDescs[0],
			sizeof(slg::ocl::Mesh) * cscene->meshDescs.size(), "Mesh description");
}

void PathOCLRenderThread::InitMaterials() {
	const size_t materialsCount = renderEngine->compiledScene->mats.size();
	AllocOCLBufferRO(&materialsBuff, &renderEngine->compiledScene->mats[0],
			sizeof(slg::ocl::Material) * materialsCount, "Materials");

	const u_int meshCount = renderEngine->compiledScene->meshMats.size();
	AllocOCLBufferRO(&meshMatsBuff, &renderEngine->compiledScene->meshMats[0],
			sizeof(u_int) * meshCount, "Mesh material index");
}

void PathOCLRenderThread::InitTextures() {
	const size_t texturesCount = renderEngine->compiledScene->texs.size();
	AllocOCLBufferRO(&texturesBuff, &renderEngine->compiledScene->texs[0],
			sizeof(slg::ocl::Texture) * texturesCount, "Textures");
}

void PathOCLRenderThread::InitTriangleAreaLights() {
	CompiledScene *cscene = renderEngine->compiledScene;

	if (cscene->triLightDefs.size() > 0) {
		AllocOCLBufferRO(&triLightDefsBuff, &cscene->triLightDefs[0],
			sizeof(slg::ocl::TriangleLight) * cscene->triLightDefs.size(), "Triangle AreaLights");
		AllocOCLBufferRO(&meshTriLightDefsOffsetBuff, &cscene->meshTriLightDefsOffset[0],
			sizeof(u_int) * cscene->meshTriLightDefsOffset.size(), "Triangle AreaLights offsets");
	} else {
		FreeOCLBuffer(&triLightDefsBuff);
		FreeOCLBuffer(&meshTriLightDefsOffsetBuff);
	}
}

void PathOCLRenderThread::InitInfiniteLight() {
	CompiledScene *cscene = renderEngine->compiledScene;

	if (cscene->infiniteLight)
		AllocOCLBufferRO(&infiniteLightBuff, cscene->infiniteLight,
			sizeof(slg::ocl::InfiniteLight), "InfiniteLight");
	else
		FreeOCLBuffer(&infiniteLightBuff);
}

void PathOCLRenderThread::InitSunLight() {
	CompiledScene *cscene = renderEngine->compiledScene;

	if (cscene->sunLight)
		AllocOCLBufferRO(&sunLightBuff, cscene->sunLight,
			sizeof(slg::ocl::SunLight), "SunLight");
	else
		FreeOCLBuffer(&sunLightBuff);
}

void PathOCLRenderThread::InitSkyLight() {
	CompiledScene *cscene = renderEngine->compiledScene;

	if (cscene->skyLight)
		AllocOCLBufferRO(&skyLightBuff, cscene->skyLight,
			sizeof(slg::ocl::SkyLight), "SkyLight");
	else
		FreeOCLBuffer(&skyLightBuff);
}

void PathOCLRenderThread::InitImageMaps() {
	CompiledScene *cscene = renderEngine->compiledScene;

	if (cscene->imageMapDescs.size() > 0) {
		AllocOCLBufferRO(&imageMapDescsBuff, &cscene->imageMapDescs[0],
				sizeof(slg::ocl::ImageMap) * cscene->imageMapDescs.size(), "ImageMaps description");

		// Free unused pages
		for (u_int i = cscene->imageMapMemBlocks.size(); i < imageMapsBuff.size(); ++i)
			FreeOCLBuffer(&imageMapsBuff[i]);
		imageMapsBuff.resize(cscene->imageMapMemBlocks.size(), NULL);

		for (u_int i = 0; i < imageMapsBuff.size(); ++i) {
			AllocOCLBufferRO(&(imageMapsBuff[i]), &(cscene->imageMapMemBlocks[i][0]),
					sizeof(float) * cscene->imageMapMemBlocks[i].size(), "ImageMaps");
		}
	} else {
		FreeOCLBuffer(&imageMapDescsBuff);
		for (u_int i = 0; i < imageMapsBuff.size(); ++i)
			FreeOCLBuffer(&imageMapsBuff[i]);
		imageMapsBuff.resize(0);
	}
}

void PathOCLRenderThread::InitKernels() {
	//--------------------------------------------------------------------------
	// Compile kernels
	//--------------------------------------------------------------------------

	CompiledScene *cscene = renderEngine->compiledScene;
	cl::Context &oclContext = intersectionDevice->GetOpenCLContext();
	cl::Device &oclDevice = intersectionDevice->GetOpenCLDevice();

	// Set #define symbols
	stringstream ss;
	ss.precision(6);
	ss << scientific <<
			" -D LUXRAYS_OPENCL_KERNEL" <<
			" -D SLG_OPENCL_KERNEL" <<
			" -D PARAM_TASK_COUNT=" << renderEngine->taskCount <<
			" -D PARAM_IMAGE_WIDTH=" << renderEngine->film->GetWidth() <<
			" -D PARAM_IMAGE_HEIGHT=" << renderEngine->film->GetHeight() <<
			" -D PARAM_RAY_EPSILON_MIN=" << MachineEpsilon::GetMin() << "f"
			" -D PARAM_RAY_EPSILON_MAX=" << MachineEpsilon::GetMax() << "f"
			" -D PARAM_MAX_PATH_DEPTH=" << renderEngine->maxPathDepth <<
			" -D PARAM_RR_DEPTH=" << renderEngine->rrDepth <<
			" -D PARAM_RR_CAP=" << renderEngine->rrImportanceCap << "f"
			;

	switch (intersectionDevice->GetAccelerator()->GetType()) {
		case ACCEL_BVH:
			ss << " -D PARAM_ACCEL_BVH";
			break;
		case ACCEL_QBVH:
			ss << " -D PARAM_ACCEL_QBVH";
			break;
		case ACCEL_MQBVH:
			ss << " -D PARAM_ACCEL_MQBVH";
			break;
		case ACCEL_MBVH:
			ss << " -D PARAM_ACCEL_MBVH";
			break;
		default:
			throw new std::runtime_error("Unknown accelerator in PathOCLRenderThread::InitKernels()");
	}

	if (normalsBuff)
		ss << " -D PARAM_HAS_NORMALS_BUFFER";
	if (uvsBuff)
		ss << " -D PARAM_HAS_UVS_BUFFER";
	if (colsBuff)
		ss << " -D PARAM_HAS_COLS_BUFFER";
	if (alphasBuff)
		ss << " -D PARAM_HAS_ALPHAS_BUFFER";

	if (cscene->IsTextureCompiled(CONST_FLOAT))
		ss << " -D PARAM_ENABLE_TEX_CONST_FLOAT";
	if (cscene->IsTextureCompiled(CONST_FLOAT3))
		ss << " -D PARAM_ENABLE_TEX_CONST_FLOAT3";
	if (cscene->IsTextureCompiled(IMAGEMAP))
		ss << " -D PARAM_ENABLE_TEX_IMAGEMAP";
	if (cscene->IsTextureCompiled(SCALE_TEX))
		ss << " -D PARAM_ENABLE_TEX_SCALE";
	if (cscene->IsTextureCompiled(FRESNEL_APPROX_N))
		ss << " -D PARAM_ENABLE_FRESNEL_APPROX_N";
	if (cscene->IsTextureCompiled(FRESNEL_APPROX_K))
		ss << " -D PARAM_ENABLE_FRESNEL_APPROX_K";
	if (cscene->IsTextureCompiled(CHECKERBOARD2D))
		ss << " -D PARAM_ENABLE_CHECKERBOARD2D";
	if (cscene->IsTextureCompiled(CHECKERBOARD3D))
		ss << " -D PARAM_ENABLE_CHECKERBOARD3D";
	if (cscene->IsTextureCompiled(MIX_TEX))
		ss << " -D PARAM_ENABLE_TEX_MIX";
	if (cscene->IsTextureCompiled(FBM_TEX))
		ss << " -D PARAM_ENABLE_FBM_TEX";
	if (cscene->IsTextureCompiled(MARBLE))
		ss << " -D PARAM_ENABLE_MARBLE";
	if (cscene->IsTextureCompiled(DOTS))
		ss << " -D PARAM_ENABLE_DOTS";
	if (cscene->IsTextureCompiled(BRICK))
		ss << " -D PARAM_ENABLE_BRICK";
	if (cscene->IsTextureCompiled(ADD_TEX))
		ss << " -D PARAM_ENABLE_TEX_ADD";
	if (cscene->IsTextureCompiled(WINDY))
		ss << " -D PARAM_ENABLE_WINDY";
	if (cscene->IsTextureCompiled(WRINKLED))
		ss << " -D PARAM_ENABLE_WRINKLED";
	if (cscene->IsTextureCompiled(UV_TEX))
		ss << " -D PARAM_ENABLE_TEX_UV";
	if (cscene->IsTextureCompiled(BAND_TEX))
		ss << " -D PARAM_ENABLE_TEX_BAND";
	if (cscene->IsTextureCompiled(HITPOINTCOLOR))
		ss << " -D PARAM_ENABLE_TEX_HITPOINTCOLOR";
	if (cscene->IsTextureCompiled(HITPOINTALPHA))
		ss << " -D PARAM_ENABLE_TEX_HITPOINTALPHA";
	if (cscene->IsTextureCompiled(HITPOINTGREY))
		ss << " -D PARAM_ENABLE_TEX_HITPOINTGREY";

	if (cscene->IsMaterialCompiled(MATTE))
		ss << " -D PARAM_ENABLE_MAT_MATTE";
	if (cscene->IsMaterialCompiled(MIRROR))
		ss << " -D PARAM_ENABLE_MAT_MIRROR";
	if (cscene->IsMaterialCompiled(GLASS))
		ss << " -D PARAM_ENABLE_MAT_GLASS";
	if (cscene->IsMaterialCompiled(METAL))
		ss << " -D PARAM_ENABLE_MAT_METAL";
	if (cscene->IsMaterialCompiled(ARCHGLASS))
		ss << " -D PARAM_ENABLE_MAT_ARCHGLASS";
	if (cscene->IsMaterialCompiled(MIX))
		ss << " -D PARAM_ENABLE_MAT_MIX";
	if (cscene->IsMaterialCompiled(NULLMAT))
		ss << " -D PARAM_ENABLE_MAT_NULL";
	if (cscene->IsMaterialCompiled(MATTETRANSLUCENT))
		ss << " -D PARAM_ENABLE_MAT_MATTETRANSLUCENT";
	if (cscene->IsMaterialCompiled(GLOSSY2)) {
		ss << " -D PARAM_ENABLE_MAT_GLOSSY2";

		if (cscene->IsMaterialCompiled(GLOSSY2_ANISOTROPIC))
			ss << " -D PARAM_ENABLE_MAT_GLOSSY2_ANISOTROPIC";
		if (cscene->IsMaterialCompiled(GLOSSY2_ABSORPTION))
			ss << " -D PARAM_ENABLE_MAT_GLOSSY2_ABSORPTION";
		if (cscene->IsMaterialCompiled(GLOSSY2_INDEX))
			ss << " -D PARAM_ENABLE_MAT_GLOSSY2_INDEX";
		if (cscene->IsMaterialCompiled(GLOSSY2_MULTIBOUNCE))
			ss << " -D PARAM_ENABLE_MAT_GLOSSY2_MULTIBOUNCE";
	}
	if (cscene->IsMaterialCompiled(METAL2)) {
		ss << " -D PARAM_ENABLE_MAT_METAL2";
		if (cscene->IsMaterialCompiled(METAL2_ANISOTROPIC))
			ss << " -D PARAM_ENABLE_MAT_METAL2_ANISOTROPIC";
	}
	if (cscene->IsMaterialCompiled(ROUGHGLASS)) {
		ss << " -D PARAM_ENABLE_MAT_ROUGHGLASS";
		if (cscene->IsMaterialCompiled(ROUGHGLASS_ANISOTROPIC))
			ss << " -D PARAM_ENABLE_MAT_ROUGHGLASS_ANISOTROPIC";
	}

	if (cscene->RequiresPassThrough())
		ss << " -D PARAM_HAS_PASSTHROUGH";
	
	if (cscene->camera.lensRadius > 0.f)
		ss << " -D PARAM_CAMERA_HAS_DOF";

	if (infiniteLightBuff)
		ss << " -D PARAM_HAS_INFINITELIGHT";

	if (skyLightBuff)
		ss << " -D PARAM_HAS_SKYLIGHT";

	if (sunLightBuff) {
		ss << " -D PARAM_HAS_SUNLIGHT";

		if (!triLightDefsBuff) {
			ss <<
				" -D PARAM_DIRECT_LIGHT_SAMPLING" <<
				" -D PARAM_DL_LIGHT_COUNT=0"
				;
		}
	}

	if (triLightDefsBuff) {
		ss <<
				" -D PARAM_DIRECT_LIGHT_SAMPLING" <<
				" -D PARAM_DL_LIGHT_COUNT=" << renderEngine->compiledScene->triLightDefs.size()
				;
	}

	if (imageMapDescsBuff) {
		ss << " -D PARAM_HAS_IMAGEMAPS";
		if (imageMapsBuff.size() > 5)
			throw std::runtime_error("Too many memory pages required for image maps");
		for (u_int i = 0; i < imageMapsBuff.size(); ++i)
			ss << " -D PARAM_IMAGEMAPS_PAGE_" << i;
	}

	if (renderEngine->compiledScene->useBumpMapping)
		ss << " -D PARAM_HAS_BUMPMAPS";
	if (renderEngine->compiledScene->useNormalMapping)
		ss << " -D PARAM_HAS_NORMALMAPS";

	const slg::ocl::Filter *filter = renderEngine->filter;
	switch (filter->type) {
		case slg::ocl::FILTER_NONE:
			ss << " -D PARAM_IMAGE_FILTER_TYPE=0";
			break;
		case slg::ocl::FILTER_BOX:
			ss << " -D PARAM_IMAGE_FILTER_TYPE=1" <<
					" -D PARAM_IMAGE_FILTER_WIDTH_X=" << filter->box.widthX << "f" <<
					" -D PARAM_IMAGE_FILTER_WIDTH_Y=" << filter->box.widthY << "f";
			break;
		case slg::ocl::FILTER_GAUSSIAN:
			ss << " -D PARAM_IMAGE_FILTER_TYPE=2" <<
					" -D PARAM_IMAGE_FILTER_WIDTH_X=" << filter->gaussian.widthX << "f" <<
					" -D PARAM_IMAGE_FILTER_WIDTH_Y=" << filter->gaussian.widthY << "f" <<
					" -D PARAM_IMAGE_FILTER_GAUSSIAN_ALPHA=" << filter->gaussian.alpha << "f";
			break;
		case slg::ocl::FILTER_MITCHELL:
			ss << " -D PARAM_IMAGE_FILTER_TYPE=3" <<
					" -D PARAM_IMAGE_FILTER_WIDTH_X=" << filter->mitchell.widthX << "f" <<
					" -D PARAM_IMAGE_FILTER_WIDTH_Y=" << filter->mitchell.widthY << "f" <<
					" -D PARAM_IMAGE_FILTER_MITCHELL_B=" << filter->mitchell.B << "f" <<
					" -D PARAM_IMAGE_FILTER_MITCHELL_C=" << filter->mitchell.C << "f";
			break;
		default:
			assert (false);
	}

	if (renderEngine->usePixelAtomics)
		ss << " -D PARAM_USE_PIXEL_ATOMICS";

	const slg::ocl::Sampler *sampler = renderEngine->sampler;
	switch (sampler->type) {
		case slg::ocl::RANDOM:
			ss << " -D PARAM_SAMPLER_TYPE=0";
			break;
		case slg::ocl::METROPOLIS:
			ss << " -D PARAM_SAMPLER_TYPE=1" <<
					" -D PARAM_SAMPLER_METROPOLIS_LARGE_STEP_RATE=" << sampler->metropolis.largeMutationProbability << "f" <<
					" -D PARAM_SAMPLER_METROPOLIS_IMAGE_MUTATION_RANGE=" << sampler->metropolis.imageMutationRange << "f" <<
					" -D PARAM_SAMPLER_METROPOLIS_MAX_CONSECUTIVE_REJECT=" << sampler->metropolis.maxRejects;
			break;
		case slg::ocl::SOBOL:
			ss << " -D PARAM_SAMPLER_TYPE=2" <<
					" -D PARAM_SAMPLER_SOBOL_STARTOFFSET=" << SOBOL_STARTOFFSET <<
					" -D PARAM_SAMPLER_SOBOL_MAXDEPTH=" << max(SOBOL_MAXDEPTH, renderEngine->maxPathDepth);
			break;
		default:
			assert (false);
	}

	if (renderEngine->film->IsAlphaChannelEnabled())
		ss << " -D PARAM_ENABLE_ALPHA_CHANNEL";

	// Some information about our place in the universe...
	ss << " -D PARAM_DEVICE_INDEX=" << threadIndex;
	ss << " -D PARAM_DEVICE_COUNT=" << renderEngine->intersectionDevices.size();

	//--------------------------------------------------------------------------
	// PARAMs used only by RTPATHOCL
	//--------------------------------------------------------------------------

	// Tonemapping is done on device only by RTPATHOCL
	if (renderEngine->film->GetToneMapParams()->GetType() == TONEMAP_LINEAR) {
		const LinearToneMapParams *ltm = (const LinearToneMapParams *)renderEngine->film->GetToneMapParams();
		ss << " -D PARAM_TONEMAP_LINEAR_SCALE=" << ltm->scale << "f";
	} else
		ss << " -D PARAM_TONEMAP_LINEAR_SCALE=1.f";

	ss << " -D PARAM_GAMMA=" << renderEngine->film->GetGamma() << "f";
	
	//--------------------------------------------------------------------------

	// Check the OpenCL vendor and use some specific compiler options
#if defined(__APPLE__)
	ss << " -D __APPLE_CL__";
#endif
	//--------------------------------------------------------------------------

	const double tStart = WallClockTime();

	// Check if I have to recompile the kernels
	string newKernelParameters = ss.str();
	if (kernelsParameters != newKernelParameters) {
		kernelsParameters = newKernelParameters;

		string sobolLookupTable;
		if (sampler->type == slg::ocl::SOBOL) {
			// Generate the Sobol vectors
			SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Sobol table size: " << sampleDimensions * SOBOL_BITS);
			u_int *directions = new u_int[sampleDimensions * SOBOL_BITS];

			SobolGenerateDirectionVectors(directions, sampleDimensions);

			stringstream sobolTableSS;
			sobolTableSS << "#line 2 \"Sobol Table in pathoclthread.cpp\"\n";
			sobolTableSS << "__constant uint SOBOL_DIRECTIONS[" << sampleDimensions * SOBOL_BITS << "] = {\n";
			for (u_int i = 0; i < sampleDimensions * SOBOL_BITS; ++i) {
				if (i != 0)
					sobolTableSS << ", ";
				sobolTableSS << directions[i] << "u";
			}
			sobolTableSS << "};\n";

			sobolLookupTable = sobolTableSS.str();

			delete directions;
		}

		// Compile sources
		stringstream ssKernel;
		ssKernel <<
			slg::ocl::KernelSource_luxrays_types <<
			slg::ocl::KernelSource_uv_types <<
			slg::ocl::KernelSource_point_types <<
			slg::ocl::KernelSource_vector_types <<
			slg::ocl::KernelSource_normal_types <<
			slg::ocl::KernelSource_triangle_types <<
			slg::ocl::KernelSource_ray_types <<
			// OpenCL Types
			slg::ocl::KernelSource_epsilon_types <<
			slg::ocl::KernelSource_spectrum_types <<
			slg::ocl::KernelSource_frame_types <<
			slg::ocl::KernelSource_matrix4x4_types <<
			slg::ocl::KernelSource_transform_types <<
			slg::ocl::KernelSource_randomgen_types <<
			slg::ocl::KernelSource_trianglemesh_types <<
			slg::ocl::KernelSource_hitpoint_types <<
			slg::ocl::KernelSource_mapping_types <<
			slg::ocl::KernelSource_texture_types <<
			slg::ocl::KernelSource_material_types <<
			slg::ocl::KernelSource_bsdf_types <<
			slg::ocl::KernelSource_sampler_types <<
			slg::ocl::KernelSource_filter_types <<
			slg::ocl::KernelSource_camera_types <<
			slg::ocl::KernelSource_light_types <<
			// OpenCL Funcs
			slg::ocl::KernelSource_epsilon_funcs <<
			slg::ocl::KernelSource_utils_funcs <<
			slg::ocl::KernelSource_vector_funcs <<
			slg::ocl::KernelSource_ray_funcs <<
			slg::ocl::KernelSource_spectrum_funcs <<
			slg::ocl::KernelSource_matrix4x4_funcs <<
			slg::ocl::KernelSource_mc_funcs <<
			slg::ocl::KernelSource_frame_funcs <<
			slg::ocl::KernelSource_transform_funcs <<
			slg::ocl::KernelSource_randomgen_funcs <<
			slg::ocl::KernelSource_triangle_funcs <<
			slg::ocl::KernelSource_trianglemesh_funcs <<
			slg::ocl::KernelSource_mapping_funcs <<
			slg::ocl::KernelSource_texture_funcs <<
			slg::ocl::KernelSource_material_funcs <<
			slg::ocl::KernelSource_camera_funcs <<
			slg::ocl::KernelSource_light_funcs <<
			slg::ocl::KernelSource_filter_funcs <<
			sobolLookupTable <<
			slg::ocl::KernelSource_sampler_funcs <<
			slg::ocl::KernelSource_bsdf_funcs <<
			slg::ocl::KernelSource_scene_funcs <<
			// SLG Kernels
			slg::ocl::KernelSource_datatypes <<
			slg::ocl::KernelSource_pathocl_kernels <<
			slg::ocl::KernelSource_rtpathocl_kernels;
		string kernelSource = ssKernel.str();

		SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Defined symbols: " << kernelsParameters);
		SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Compiling kernels ");

		bool cached;
		cl::STRING_CLASS error;
		cl::Program *program = kernelCache->Compile(oclContext, oclDevice,
				kernelsParameters, kernelSource,
				&cached, &error);

		if (!program) {
			SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] PathOCL kernel compilation error" << std::endl << error);

			throw std::runtime_error("PathOCL kernel compilation error");
		}

		if (cached) {
			SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Kernels cached");
		} else {
			SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Kernels not cached");
		}

		//----------------------------------------------------------------------
		// Init kernel
		//----------------------------------------------------------------------

		delete initKernel;
		SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Compiling Init Kernel");
		initKernel = new cl::Kernel(*program, "Init");
		initKernel->getWorkGroupInfo<size_t>(oclDevice, CL_KERNEL_WORK_GROUP_SIZE, &initWorkGroupSize);
		if (intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize() > 0)
			initWorkGroupSize = intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize();

		//--------------------------------------------------------------------------
		// InitFB kernel
		//--------------------------------------------------------------------------

		delete initFBKernel;
		SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Compiling InitFrameBuffer Kernel");
		initFBKernel = new cl::Kernel(*program, "InitFrameBuffer");
		initFBKernel->getWorkGroupInfo<size_t>(oclDevice, CL_KERNEL_WORK_GROUP_SIZE, &initFBWorkGroupSize);
		if (intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize() > 0)
			initFBWorkGroupSize = intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize();

		//----------------------------------------------------------------------
		// AdvancePaths kernel
		//----------------------------------------------------------------------

		delete advancePathsKernel;
		SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Compiling AdvancePaths Kernel");
		advancePathsKernel = new cl::Kernel(*program, "AdvancePaths");
		advancePathsKernel->getWorkGroupInfo<size_t>(oclDevice, CL_KERNEL_WORK_GROUP_SIZE, &advancePathsWorkGroupSize);
		if (intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize() > 0)
			advancePathsWorkGroupSize = intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize();

		//----------------------------------------------------------------------
		// The following kernels are used only by RTPathOCL
		//----------------------------------------------------------------------

		if (dynamic_cast<RTPathOCLRenderEngine *>(renderEngine)) {
			//------------------------------------------------------------------
			// ClearFrameBuffer kernel
			//------------------------------------------------------------------

			delete clearFBKernel;
			SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Compiling ClearFrameBuffer Kernel");
			clearFBKernel = new cl::Kernel(*program, "ClearFrameBuffer");
			clearFBKernel->getWorkGroupInfo<size_t>(oclDevice, CL_KERNEL_WORK_GROUP_SIZE, &clearFBWorkGroupSize);
			if (intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize() > 0)
				clearFBWorkGroupSize = intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize();

			//------------------------------------------------------------------
			// InitRTFrameBuffer kernel
			//------------------------------------------------------------------

			delete clearSBKernel;
			SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Compiling ClearScreenBuffer Kernel");
			clearSBKernel = new cl::Kernel(*program, "ClearScreenBuffer");
			clearSBKernel->getWorkGroupInfo<size_t>(oclDevice, CL_KERNEL_WORK_GROUP_SIZE, &clearSBWorkGroupSize);
			if (intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize() > 0)
				clearSBWorkGroupSize = intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize();

			//------------------------------------------------------------------
			// MergeFrameBuffer kernel
			//------------------------------------------------------------------

			delete mergeFBKernel;
			SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Compiling MergeFrameBuffer Kernel");
			mergeFBKernel = new cl::Kernel(*program, "MergeFrameBuffer");
			mergeFBKernel->getWorkGroupInfo<size_t>(oclDevice, CL_KERNEL_WORK_GROUP_SIZE, &mergeFBWorkGroupSize);
			if (intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize() > 0)
				mergeFBWorkGroupSize = intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize();

			//------------------------------------------------------------------
			// NormalizeFrameBuffer kernel
			//------------------------------------------------------------------

			delete normalizeFBKernel;
			SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Compiling NormalizeFrameBuffer Kernel");
			normalizeFBKernel = new cl::Kernel(*program, "NormalizeFrameBuffer");
			normalizeFBKernel->getWorkGroupInfo<size_t>(oclDevice, CL_KERNEL_WORK_GROUP_SIZE, &normalizeFBWorkGroupSize);
			if (intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize() > 0)
				normalizeFBWorkGroupSize = intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize();

			//------------------------------------------------------------------
			// Gaussian blur frame buffer filter kernel
			//------------------------------------------------------------------

			delete applyBlurFilterXR1Kernel;
			SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Compiling ApplyGaussianBlurFilterXR1 Kernel");
			applyBlurFilterXR1Kernel = new cl::Kernel(*program, "ApplyGaussianBlurFilterXR1");
			applyBlurFilterXR1Kernel->getWorkGroupInfo<size_t>(oclDevice, CL_KERNEL_WORK_GROUP_SIZE, &applyBlurFilterXR1WorkGroupSize);
			if (intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize() > 0)
				applyBlurFilterXR1WorkGroupSize = intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize();

			delete applyBlurFilterYR1Kernel;
			SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Compiling ApplyGaussianBlurFilterYR1 Kernel");
			applyBlurFilterYR1Kernel = new cl::Kernel(*program, "ApplyGaussianBlurFilterYR1");
			applyBlurFilterYR1Kernel->getWorkGroupInfo<size_t>(oclDevice, CL_KERNEL_WORK_GROUP_SIZE, &applyBlurFilterYR1WorkGroupSize);
			if (intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize() > 0)
				applyBlurFilterYR1WorkGroupSize = intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize();

			//------------------------------------------------------------------
			// ToneMapLinear kernel
			//------------------------------------------------------------------

			delete toneMapLinearKernel;
			SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Compiling ToneMapLinear Kernel");
			toneMapLinearKernel = new cl::Kernel(*program, "ToneMapLinear");
			toneMapLinearKernel->getWorkGroupInfo<size_t>(oclDevice, CL_KERNEL_WORK_GROUP_SIZE, &toneMapLinearWorkGroupSize);
			if (intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize() > 0)
				toneMapLinearWorkGroupSize = intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize();

			//------------------------------------------------------------------
			// UpdateScreenBuffer kernel
			//------------------------------------------------------------------

			delete updateScreenBufferKernel;
			SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Compiling UpdateScreenBuffer Kernel");
			updateScreenBufferKernel = new cl::Kernel(*program, "UpdateScreenBuffer");
			updateScreenBufferKernel->getWorkGroupInfo<size_t>(oclDevice, CL_KERNEL_WORK_GROUP_SIZE, &updateScreenBufferWorkGroupSize);
			if (intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize() > 0)
				updateScreenBufferWorkGroupSize = intersectionDevice->GetDeviceDesc()->GetForceWorkGroupSize();
		}

		//----------------------------------------------------------------------

		const double tEnd = WallClockTime();
		SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Kernels compilation time: " << int((tEnd - tStart) * 1000.0) << "ms");

		delete program;
	} else
		SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Using cached kernels");
}

void PathOCLRenderThread::InitRender() {
	Scene *scene = renderEngine->renderConfig->scene;
	const u_int taskCount = renderEngine->taskCount;
	double tStart, tEnd;

	// In case renderEngine->taskCount has changed
	delete gpuTaskStats;
	gpuTaskStats = new slg::ocl::GPUTaskStats[renderEngine->taskCount];

	//--------------------------------------------------------------------------
	// FrameBuffer definition
	//--------------------------------------------------------------------------

	InitFrameBuffer();

	//--------------------------------------------------------------------------
	// Camera definition
	//--------------------------------------------------------------------------

	InitCamera();

	//--------------------------------------------------------------------------
	// Scene geometry
	//--------------------------------------------------------------------------

	InitGeometry();

	//--------------------------------------------------------------------------
	// Image maps
	//--------------------------------------------------------------------------

	InitImageMaps();

	//--------------------------------------------------------------------------
	// Texture definitions
	//--------------------------------------------------------------------------

	InitTextures();

	//--------------------------------------------------------------------------
	// Material definitions
	//--------------------------------------------------------------------------

	InitMaterials();

	//--------------------------------------------------------------------------
	// Translate triangle area lights
	//--------------------------------------------------------------------------

	InitTriangleAreaLights();

	//--------------------------------------------------------------------------
	// Check if there is an infinite light source
	//--------------------------------------------------------------------------

	InitInfiniteLight();

	//--------------------------------------------------------------------------
	// Check if there is an sun light source
	//--------------------------------------------------------------------------

	InitSunLight();

	//--------------------------------------------------------------------------
	// Check if there is an sky light source
	//--------------------------------------------------------------------------

	InitSkyLight();

	const u_int triAreaLightCount = renderEngine->compiledScene->triLightDefs.size();
	if (!skyLightBuff && !sunLightBuff && !infiniteLightBuff && (triAreaLightCount == 0))
		throw runtime_error("There are no light sources supported by PathOCL in the scene");

	//--------------------------------------------------------------------------
	// Allocate Ray/RayHit buffers
	//--------------------------------------------------------------------------

	tStart = WallClockTime();

	AllocOCLBufferRW(&raysBuff, sizeof(Ray) * taskCount, "Ray");
	AllocOCLBufferRW(&hitsBuff, sizeof(RayHit) * taskCount, "RayHit");

	//--------------------------------------------------------------------------
	// Allocate GPU task buffers
	//--------------------------------------------------------------------------

	const bool hasPassThrough = renderEngine->compiledScene->RequiresPassThrough();

	// Add Seed memory size
	size_t gpuTaksSize = sizeof(slg::ocl::Seed);

	// Add Sample memory size
	if (renderEngine->sampler->type == slg::ocl::RANDOM) {
		gpuTaksSize += sizeof(Spectrum);

		if (alphaFrameBufferBuff)
			gpuTaksSize += sizeof(float);
	} else if (renderEngine->sampler->type == slg::ocl::METROPOLIS) {
		gpuTaksSize += 2 * sizeof(Spectrum) + 3 * sizeof(float) + 5 * sizeof(u_int);
		
		if (alphaFrameBufferBuff)
			gpuTaksSize += sizeof(float);
	} else if (renderEngine->sampler->type == slg::ocl::SOBOL) {
		gpuTaksSize += 2 * sizeof(float) + 2 * sizeof(u_int) + sizeof(Spectrum);

		if (alphaFrameBufferBuff)
			gpuTaksSize += sizeof(float);
	} else
		throw std::runtime_error("Unknown sampler.type: " + boost::lexical_cast<std::string>(renderEngine->sampler->type));

	// Add PathStateBase memory size
	gpuTaksSize += sizeof(int) + sizeof(u_int) + sizeof(Spectrum);

	// Add PathStateBase.BSDF.HitPoint memory size
	size_t hitPointSize = sizeof(Vector) + sizeof(Point) + sizeof(UV) + 2 * sizeof(Normal);
	if (renderEngine->compiledScene->IsTextureCompiled(HITPOINTCOLOR) ||
			renderEngine->compiledScene->IsTextureCompiled(HITPOINTGREY))
		hitPointSize += sizeof(Spectrum);
	if (renderEngine->compiledScene->IsTextureCompiled(HITPOINTALPHA))
		hitPointSize += sizeof(float);
	if (hasPassThrough)
		hitPointSize += sizeof(float);

	// Add PathStateBase.BSDF memory size
	size_t bsdfSize = hitPointSize;
	// Add PathStateBase.BSDF.materialIndex memory size
	bsdfSize += sizeof(u_int);
	// Add PathStateBase.BSDF.triangleLightSourceIndex memory size
	if (triAreaLightCount > 0)
		bsdfSize += sizeof(u_int);
	// Add PathStateBase.BSDF.Frame memory size
	bsdfSize += sizeof(slg::ocl::Frame);
	gpuTaksSize += bsdfSize;

	// Add PathStateDirectLight memory size
	if ((triAreaLightCount > 0) || sunLightBuff) {
		gpuTaksSize += sizeof(Spectrum) + sizeof(float) + sizeof(int);

		// Add PathStateDirectLight.tmpHitPoint memory size
		if (triAreaLightCount > 0)
			gpuTaksSize += hitPointSize;

		// Add PathStateDirectLightPassThrough memory size
		if (hasPassThrough)
			gpuTaksSize += sizeof(float) + bsdfSize;
	}

	SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Size of a GPUTask: " << gpuTaksSize << "bytes");
	AllocOCLBufferRW(&tasksBuff, gpuTaksSize * taskCount, "GPUTask");

	//--------------------------------------------------------------------------
	// Allocate sample data buffers
	//--------------------------------------------------------------------------

	const size_t eyePathVertexDimension =
		// IDX_SCREEN_X, IDX_SCREEN_Y
		2 +
		// IDX_EYE_PASSTROUGHT
		(hasPassThrough ? 1 : 0) +
		// IDX_DOF_X, IDX_DOF_Y
		((scene->camera->lensRadius > 0.f) ? 2 : 0);
	const size_t PerPathVertexDimension =
		// IDX_PASSTHROUGH,
		(hasPassThrough ? 1 : 0) +
		// IDX_BSDF_X, IDX_BSDF_Y
		2 +
		// IDX_DIRECTLIGHT_X, IDX_DIRECTLIGHT_Y, IDX_DIRECTLIGHT_Z, IDX_DIRECTLIGHT_W, IDX_DIRECTLIGHT_A
		(((triAreaLightCount > 0) || sunLightBuff) ? (4 + (hasPassThrough ? 1 : 0)) : 0) +
		// IDX_RR
		1;
	sampleDimensions = eyePathVertexDimension + PerPathVertexDimension * renderEngine->maxPathDepth;

	size_t uDataSize;
	if ((renderEngine->sampler->type == slg::ocl::RANDOM) ||
			(renderEngine->sampler->type == slg::ocl::SOBOL)) {
		// Only IDX_SCREEN_X, IDX_SCREEN_Y
		uDataSize = sizeof(float) * 2;
		
		if (renderEngine->sampler->type == slg::ocl::SOBOL) {
			// Limit the number of dimension where I use Sobol sequence (after, I switch
			// to Random sampler.
			sampleDimensions = eyePathVertexDimension + PerPathVertexDimension * max(SOBOL_MAXDEPTH, renderEngine->maxPathDepth);
		}
	} else if (renderEngine->sampler->type == slg::ocl::METROPOLIS) {
		// Metropolis needs 2 sets of samples, the current and the proposed mutation
		uDataSize = 2 * sizeof(float) * sampleDimensions;
	} else
		throw std::runtime_error("Unknown sampler.type: " + boost::lexical_cast<std::string>(renderEngine->sampler->type));

	SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Sample dimensions: " << sampleDimensions);
	SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Size of a SampleData: " << uDataSize << "bytes");

	// TOFIX
	AllocOCLBufferRW(&sampleDataBuff, uDataSize * taskCount + 1, "SampleData"); // +1 to avoid METROPOLIS + Intel\AMD OpenCL crash

	//--------------------------------------------------------------------------
	// Allocate GPU task statistic buffers
	//--------------------------------------------------------------------------

	AllocOCLBufferRW(&taskStatsBuff, sizeof(slg::ocl::GPUTaskStats) * taskCount, "GPUTask Stats");

	tEnd = WallClockTime();
	SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] OpenCL buffer creation time: " << int((tEnd - tStart) * 1000.0) << "ms");

	//--------------------------------------------------------------------------
	// Compile kernels
	//--------------------------------------------------------------------------

	InitKernels();

	//--------------------------------------------------------------------------
	// Initialize
	//--------------------------------------------------------------------------

	// Set kernel arguments
	SetKernelArgs();

	cl::CommandQueue &oclQueue = intersectionDevice->GetOpenCLQueue();

	// Clear the frame buffer
	oclQueue.enqueueNDRangeKernel(*initFBKernel, cl::NullRange,
			cl::NDRange(RoundUp<u_int>(frameBufferPixelCount, initFBWorkGroupSize)),
			cl::NDRange(initFBWorkGroupSize));

	// Initialize the tasks buffer
	oclQueue.enqueueNDRangeKernel(*initKernel, cl::NullRange,
			cl::NDRange(taskCount), cl::NDRange(initWorkGroupSize));
	oclQueue.finish();

	// Reset statistics in order to be more accurate
	intersectionDevice->ResetPerformaceStats();
}

void PathOCLRenderThread::SetKernelArgs() {
	// Set OpenCL kernel arguments

	// OpenCL kernel setArg() is the only no thread safe function in OpenCL 1.1 so
	// I need to use a mutex here
	boost::unique_lock<boost::mutex> lock(renderEngine->setKernelArgsMutex);

	//--------------------------------------------------------------------------
	// advancePathsKernel
	//--------------------------------------------------------------------------

	u_int argIndex = 0;
	advancePathsKernel->setArg(argIndex++, *tasksBuff);
	advancePathsKernel->setArg(argIndex++, *taskStatsBuff);
	advancePathsKernel->setArg(argIndex++, *sampleDataBuff);
	advancePathsKernel->setArg(argIndex++, *raysBuff);
	advancePathsKernel->setArg(argIndex++, *hitsBuff);
	advancePathsKernel->setArg(argIndex++, *frameBufferBuff);
	advancePathsKernel->setArg(argIndex++, *materialsBuff);
	advancePathsKernel->setArg(argIndex++, *texturesBuff);
	advancePathsKernel->setArg(argIndex++, *meshMatsBuff);
	advancePathsKernel->setArg(argIndex++, *meshIDBuff);
	advancePathsKernel->setArg(argIndex++, *meshDescsBuff);
	advancePathsKernel->setArg(argIndex++, *vertsBuff);
	if (normalsBuff)
		advancePathsKernel->setArg(argIndex++, *normalsBuff);
	if (uvsBuff)
		advancePathsKernel->setArg(argIndex++, *uvsBuff);
	if (colsBuff)
		advancePathsKernel->setArg(argIndex++, *colsBuff);
	if (alphasBuff)
		advancePathsKernel->setArg(argIndex++, *alphasBuff);
	advancePathsKernel->setArg(argIndex++, *trianglesBuff);
	advancePathsKernel->setArg(argIndex++, *cameraBuff);
	if (infiniteLightBuff)
		advancePathsKernel->setArg(argIndex++, *infiniteLightBuff);
	if (sunLightBuff)
		advancePathsKernel->setArg(argIndex++, *sunLightBuff);
	if (skyLightBuff)
		advancePathsKernel->setArg(argIndex++, *skyLightBuff);
	if (triLightDefsBuff) {
		advancePathsKernel->setArg(argIndex++, *triLightDefsBuff);
		advancePathsKernel->setArg(argIndex++, *meshTriLightDefsOffsetBuff);
	}
	if (imageMapDescsBuff) {
		advancePathsKernel->setArg(argIndex++, *imageMapDescsBuff);

		for (u_int i = 0; i < imageMapsBuff.size(); ++i)
			advancePathsKernel->setArg(argIndex++, *(imageMapsBuff[i]));
	}
	if (alphaFrameBufferBuff)
		advancePathsKernel->setArg(argIndex++, *alphaFrameBufferBuff);

	//--------------------------------------------------------------------------
	// initFBKernel
	//--------------------------------------------------------------------------

	initFBKernel->setArg(0, *frameBufferBuff);
	if (alphaFrameBufferBuff)
		initFBKernel->setArg(1, *alphaFrameBufferBuff);

	//--------------------------------------------------------------------------
	// initKernel
	//--------------------------------------------------------------------------

	argIndex = 0;
	initKernel->setArg(argIndex++, renderEngine->seedBase + threadIndex * renderEngine->taskCount);
	initKernel->setArg(argIndex++, *tasksBuff);
	initKernel->setArg(argIndex++, *sampleDataBuff);
	initKernel->setArg(argIndex++, *taskStatsBuff);
	initKernel->setArg(argIndex++, *raysBuff);
	initKernel->setArg(argIndex++, *cameraBuff);
}

void PathOCLRenderThread::Start() {
	started = true;

	InitRender();
	StartRenderThread();
}

void PathOCLRenderThread::Interrupt() {
	if (renderThread)
		renderThread->interrupt();
}

void PathOCLRenderThread::Stop() {
	StopRenderThread();

	if (frameBuffer) {
		// Transfer of the frame buffer
		cl::CommandQueue &oclQueue = intersectionDevice->GetOpenCLQueue();
		oclQueue.enqueueReadBuffer(
			*frameBufferBuff,
			CL_TRUE,
			0,
			frameBufferBuff->getInfo<CL_MEM_SIZE>(),
			frameBuffer);

		// Check if I have to transfer the alpha channel too
		if (alphaFrameBufferBuff) {
			oclQueue.enqueueReadBuffer(
				*alphaFrameBufferBuff,
				CL_TRUE,
				0,
				alphaFrameBufferBuff->getInfo<CL_MEM_SIZE>(),
				alphaFrameBuffer);
		}
	}

	FreeOCLBuffer(&raysBuff);
	FreeOCLBuffer(&hitsBuff);
	FreeOCLBuffer(&tasksBuff);
	FreeOCLBuffer(&sampleDataBuff);
	FreeOCLBuffer(&taskStatsBuff);
	FreeOCLBuffer(&frameBufferBuff);
	FreeOCLBuffer(&alphaFrameBufferBuff);
	FreeOCLBuffer(&materialsBuff);
	FreeOCLBuffer(&texturesBuff);
	FreeOCLBuffer(&meshIDBuff);
	FreeOCLBuffer(&meshDescsBuff);
	FreeOCLBuffer(&meshMatsBuff);
	FreeOCLBuffer(&normalsBuff);
	FreeOCLBuffer(&uvsBuff);
	FreeOCLBuffer(&colsBuff);
	FreeOCLBuffer(&alphasBuff);
	FreeOCLBuffer(&trianglesBuff);
	FreeOCLBuffer(&vertsBuff);
	FreeOCLBuffer(&infiniteLightBuff);
	FreeOCLBuffer(&sunLightBuff);
	FreeOCLBuffer(&skyLightBuff);
	FreeOCLBuffer(&cameraBuff);
	FreeOCLBuffer(&triLightDefsBuff);
	FreeOCLBuffer(&meshTriLightDefsOffsetBuff);
	FreeOCLBuffer(&imageMapDescsBuff);
	for (u_int i = 0; i < imageMapsBuff.size(); ++i)
		FreeOCLBuffer(&imageMapsBuff[i]);
	imageMapsBuff.resize(0);

	started = false;

	// frameBuffer is delete on the destructor to allow image saving after
	// the rendering is finished
}

void PathOCLRenderThread::StartRenderThread() {
	// Create the thread for the rendering
	renderThread = new boost::thread(&PathOCLRenderThread::RenderThreadImpl, this);
}

void PathOCLRenderThread::StopRenderThread() {
	if (renderThread) {
		renderThread->interrupt();
		renderThread->join();
		delete renderThread;
		renderThread = NULL;
	}
}

void PathOCLRenderThread::BeginEdit() {
	StopRenderThread();
}

void PathOCLRenderThread::EndEdit(const EditActionList &editActions) {
	//--------------------------------------------------------------------------
	// Update OpenCL buffers
	//--------------------------------------------------------------------------

	if (editActions.Has(FILM_EDIT)) {
		// Resize the Frame Buffer
		InitFrameBuffer();
	}

	if (editActions.Has(CAMERA_EDIT)) {
		// Update Camera
		InitCamera();
	}

	if (editActions.Has(GEOMETRY_EDIT)) {
		// Update Scene Geometry
		InitGeometry();
	}

	if (editActions.Has(MATERIALS_EDIT) || editActions.Has(MATERIAL_TYPES_EDIT)) {
		// Update Scene Textures and Materials
		InitTextures();
		InitMaterials();
	}

	if  (editActions.Has(AREALIGHTS_EDIT)) {
		// Update Scene Area Lights
		InitTriangleAreaLights();
	}

	if  (editActions.Has(INFINITELIGHT_EDIT)) {
		// Update Scene Infinite Light
		InitInfiniteLight();
	}

	if  (editActions.Has(SUNLIGHT_EDIT)) {
		// Update Scene Sun Light
		InitSunLight();
	}

	if  (editActions.Has(SKYLIGHT_EDIT)) {
		// Update Scene Sun Light
		InitSkyLight();
	}

	//--------------------------------------------------------------------------
	// Recompile Kernels if required
	//--------------------------------------------------------------------------

	if (editActions.Has(FILM_EDIT) || editActions.Has(MATERIAL_TYPES_EDIT))
		InitKernels();

	if (editActions.HasAnyAction()) {
		SetKernelArgs();

		//--------------------------------------------------------------------------
		// Execute initialization kernels
		//--------------------------------------------------------------------------

		cl::CommandQueue &oclQueue = intersectionDevice->GetOpenCLQueue();

		// Clear the frame buffer
		oclQueue.enqueueNDRangeKernel(*initFBKernel, cl::NullRange,
			cl::NDRange(RoundUp<u_int>(frameBufferPixelCount, initFBWorkGroupSize)),
			cl::NDRange(initFBWorkGroupSize));

		// Initialize the tasks buffer
		oclQueue.enqueueNDRangeKernel(*initKernel, cl::NullRange,
				cl::NDRange(renderEngine->taskCount), cl::NDRange(initWorkGroupSize));
	}

	// Reset statistics in order to be more accurate
	intersectionDevice->ResetPerformaceStats();

	StartRenderThread();
}

void PathOCLRenderThread::RenderThreadImpl() {
	//SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Rendering thread started");

	cl::CommandQueue &oclQueue = intersectionDevice->GetOpenCLQueue();
	const u_int taskCount = renderEngine->taskCount;

	try {
		double startTime = WallClockTime();
		while (!boost::this_thread::interruption_requested()) {
			/*if(threadIndex == 0)
				cerr<< "[DEBUG] =================================");*/

			// Async. transfer of the frame buffer
			oclQueue.enqueueReadBuffer(
				*frameBufferBuff,
				CL_FALSE,
				0,
				frameBufferBuff->getInfo<CL_MEM_SIZE>(),
				frameBuffer);

			// Check if I have to transfer the alpha channel too
			if (alphaFrameBufferBuff) {
				// Async. transfer of the alpha channel
				oclQueue.enqueueReadBuffer(
					*alphaFrameBufferBuff,
					CL_FALSE,
					0,
					alphaFrameBufferBuff->getInfo<CL_MEM_SIZE>(),
					alphaFrameBuffer);
			}

			// Async. transfer of GPU task statistics
			oclQueue.enqueueReadBuffer(
				*(taskStatsBuff),
				CL_FALSE,
				0,
				sizeof(slg::ocl::GPUTaskStats) * taskCount,
				gpuTaskStats);

			for (;;) {
				cl::Event event;

				// Decide how many kernels to enqueue
				const u_int screenRefreshInterval = renderEngine->renderConfig->GetScreenRefreshInterval();

				u_int iterations;
				if (screenRefreshInterval <= 100)
					iterations = 1;
				else if (screenRefreshInterval <= 500)
					iterations = 2;
				else if (screenRefreshInterval <= 1000)
					iterations = 4;
				else
					iterations = 8;

				for (u_int i = 0; i < iterations; ++i) {
					// Trace rays
					if (i == 0)
						intersectionDevice->EnqueueTraceRayBuffer(*raysBuff,
								*(hitsBuff), taskCount, NULL, &event);
					else
						intersectionDevice->EnqueueTraceRayBuffer(*raysBuff,
								*(hitsBuff), taskCount, NULL, NULL);

					// Advance to next path state
					oclQueue.enqueueNDRangeKernel(*advancePathsKernel, cl::NullRange,
							cl::NDRange(taskCount), cl::NDRange(advancePathsWorkGroupSize));
				}
				oclQueue.flush();

				event.wait();
				const double elapsedTime = WallClockTime() - startTime;

				/*if(threadIndex == 0)
					cerr<< "[DEBUG] Elapsed time: " << elapsedTime * 1000.0 <<
							"ms (screenRefreshInterval: " << renderEngine->screenRefreshInterval << ")");*/

				if ((elapsedTime * 1000.0 > (double)screenRefreshInterval) ||
						boost::this_thread::interruption_requested())
					break;
			}

			startTime = WallClockTime();
		}

		//SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Rendering thread halted");
	} catch (boost::thread_interrupted) {
		SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Rendering thread halted");
	} catch (cl::Error err) {
		SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Rendering thread ERROR: " << err.what() <<
				"(" << oclErrorString(err.err()) << ")");
	}

	oclQueue.enqueueReadBuffer(
			*frameBufferBuff,
			CL_TRUE,
			0,
			frameBufferBuff->getInfo<CL_MEM_SIZE>(),
			frameBuffer);

	// Check if I have to transfer the alpha channel too
	if (alphaFrameBufferBuff) {
		oclQueue.enqueueReadBuffer(
			*alphaFrameBufferBuff,
			CL_TRUE,
			0,
			alphaFrameBufferBuff->getInfo<CL_MEM_SIZE>(),
			alphaFrameBuffer);
	}
}

#endif