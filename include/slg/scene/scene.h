/***************************************************************************
 * Copyright 1998-2018 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#ifndef _SLG_SCENE_H
#define	_SLG_SCENE_H

#include <string>
#include <iostream>
#include <fstream>

#include "luxrays/core/intersectiondevice.h"
#include "luxrays/core/accelerator.h"
#include "luxrays/core/geometry/transform.h"
#include "luxrays/core/geometry/motionsystem.h"
#include "luxrays/utils/mc.h"
#include "luxrays/utils/mcdistribution.h"
#include "luxrays/utils/properties.h"
#include "luxrays/utils/serializationutils.h"
#include "slg/core/sdl.h"
#include "slg/cameras/camera.h"
#include "slg/editaction.h"
#include "slg/lights/light.h"
#include "slg/lights/lightsourcedefs.h"
#include "slg/shapes/strands.h"
#include "slg/textures/texture.h"
#include "slg/textures/texturedefs.h"
#include "slg/textures/mapping/mapping.h"
#include "slg/materials/materialdefs.h"
#include "slg/bsdf/bsdf.h"
#include "slg/volumes/volume.h"
#include "slg/scene/sceneobjectdefs.h"
#include "slg/scene/extmeshcache.h"


namespace slg {

#define TRIANGLE_LIGHT_POSTFIX "__triangle__light__"

class Scene {
public:
	// Constructor used to create a scene by calling methods
	Scene(const float imageScale = 1.f);
	// Constructor used to create a scene from properties
	Scene(const luxrays::Properties &scnProp, const float imageScale = 1.f);
	~Scene();

	bool Intersect(luxrays::IntersectionDevice *device,
		const bool fromLight, const bool cameraRay, PathVolumeInfo *volInfo,
		const float passThrough, luxrays::Ray *ray, luxrays::RayHit *rayHit, BSDF *bsdf,
		luxrays::Spectrum *connectionThroughput, const luxrays::Spectrum *pathThroughput = NULL,
		SampleResult *sampleResult = NULL) const;

	void PreprocessCamera(const u_int filmWidth, const u_int filmHeight, const u_int *filmSubRegion);
	void Preprocess(luxrays::Context *ctx,
		const u_int filmWidth, const u_int filmHeight, const u_int *filmSubRegion,
		const bool useRTMode);

	luxrays::Properties ToProperties(const bool useRealFileName) const;

	//--------------------------------------------------------------------------
	// Methods to build and edit scene
	//--------------------------------------------------------------------------

	void DefineImageMap(ImageMap *im);
	template <class T> void DefineImageMap(const std::string &name, T *pixels, const float gamma,
		const u_int channels, const u_int width, const u_int height,
		ImageMapStorage::ChannelSelectionType selectionType,
		ImageMapStorage::WrapType wrapType) {
		ImageMap *imgMap = ImageMap::AllocImageMap<T>(gamma, channels, width, height, wrapType);
		imgMap->SetName(name);
		memcpy(imgMap->GetStorage()->GetPixelsData(), pixels, width * height * channels * sizeof(T));
		imgMap->ReverseGammaCorrection();
		imgMap->SelectChannel(selectionType);

		DefineImageMap(imgMap);

		editActions.AddAction(IMAGEMAPS_EDIT);
	}

	bool IsImageMapDefined(const std::string &imgMapName) const;

	// Mesh shape
	// Use one of the following methods, do not directly call extMeshCache.DefineExtMesh()
	void DefineMesh(luxrays::ExtMesh *mesh);
	void DefineMesh(const std::string &shapeName,
		const long plyNbVerts, const long plyNbTris,
		luxrays::Point *p, luxrays::Triangle *vi, luxrays::Normal *n, luxrays::UV *uv,
		luxrays::Spectrum *cols, float *alphas);
	void DefineMesh(const std::string &instMeshName, const std::string &meshName,
		const luxrays::Transform &trans);
	void DefineMesh(const std::string &motMeshName, const std::string &meshName,
		const luxrays::MotionSystem &ms);
	// Strands shape
	void DefineStrands(const std::string &shapeName, const luxrays::cyHairFile &strandsFile,
		const StrendsShape::TessellationType tesselType,
		const u_int adaptiveMaxDepth, const float adaptiveError,
		const u_int solidSideCount, const bool solidCapBottom, const bool solidCapTop,
		const bool useCameraPosition);

	bool IsTextureDefined(const std::string &texName) const;
	bool IsMaterialDefined(const std::string &matName) const;
	bool IsMeshDefined(const std::string &meshName) const;

	void Parse(const luxrays::Properties &props);
	void DeleteObject(const std::string &objName);
	void DeleteLight(const std::string &lightName);

	void DuplicateObject(const std::string &srcObjName, const std::string &dstObjName,
			const luxrays::Transform &trans);
	void DuplicateObject(const std::string &srcObjName, const std::string &dstObjName,
			const luxrays::MotionSystem &ms);
	void UpdateObjectMaterial(const std::string &objName, const std::string &matName);
	void UpdateObjectTransformation(const std::string &objName, const luxrays::Transform &trans);

	void RemoveUnusedImageMaps();
	void RemoveUnusedTextures();
	void RemoveUnusedMaterials();
	void RemoveUnusedMeshes();

	static Scene *LoadSerialized(const std::string &fileName);
	static void SaveSerialized(const std::string &fileName, const Scene *scene);

	//--------------------------------------------------------------------------

	// This volume is applied to rays hitting nothing
	const Volume *defaultWorldVolume;

	Camera *camera;

	ExtMeshCache extMeshCache; // Mesh objects cache
	ImageMapCache imgMapCache; // Image maps cache

	TextureDefinitions texDefs; // Texture definitions
	MaterialDefinitions matDefs; // Material definitions
	SceneObjectDefinitions objDefs; // SceneObject definitions
	LightSourceDefinitions lightDefs; // LightSource definitions

	luxrays::DataSet *dataSet;
	// The bounding sphere of the scene (including the camera)
	luxrays::BSphere sceneBSphere;

	EditActionList editActions;

	bool enableParsePrint;

	friend class boost::serialization::access;

private:
	void Init(const float imageScale);

	void ParseCamera(const luxrays::Properties &props);
	void ParseTextures(const luxrays::Properties &props);
	void ParseVolumes(const luxrays::Properties &props);
	void ParseMaterials(const luxrays::Properties &props);
	void ParseShapes(const luxrays::Properties &props);
	void ParseObjects(const luxrays::Properties &props);
	void ParseLights(const luxrays::Properties &props);

	const Texture *GetTexture(const luxrays::Property &name);

	Camera *CreateCamera(const luxrays::Properties &props);
	TextureMapping2D *CreateTextureMapping2D(const std::string &prefixName, const luxrays::Properties &props);
	TextureMapping3D *CreateTextureMapping3D(const std::string &prefixName, const luxrays::Properties &props);
	Texture *CreateTexture(const std::string &texName, const luxrays::Properties &props);
	Volume *CreateVolume(const u_int defaultVolID, const std::string &volName, const luxrays::Properties &props);
	Material *CreateMaterial(const u_int defaultMatID, const std::string &matName, const luxrays::Properties &props);
	luxrays::ExtTriangleMesh *CreateShape(const std::string &shapeName, const luxrays::Properties &props);
	SceneObject *CreateObject(const u_int defaultObjID, const std::string &objName, const luxrays::Properties &props);
	ImageMap *CreateEmissionMap(const std::string &propName, const luxrays::Properties &props);
	LightSource *CreateLightSource(const std::string &lightName, const luxrays::Properties &props);

	luxrays::ExtTriangleMesh *CreateInlinedMesh(const std::string &shapeName,
			const std::string &propName, const luxrays::Properties &props);

	template<class Archive> void save(Archive &ar, const u_int version) const;
	template<class Archive> void load(Archive &ar, const u_int version);
	BOOST_SERIALIZATION_SPLIT_MEMBER()
};

}

BOOST_CLASS_VERSION(slg::Scene, 1)

BOOST_CLASS_EXPORT_KEY(slg::Scene)

#endif	/* _SLG_SCENE_H */
