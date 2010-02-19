/***************************************************************************
 *   Copyright (C) 1998-2010 by authors (see AUTHORS.txt )                 *
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

#ifndef _SCENE_H
#define	_SCENE_H

#include <string>
#include <iostream>
#include <fstream>

#include "smalllux.h"
#include "luxrays/core/context.h"
#include "luxrays/utils/core/exttrianglemesh.h"
#include "camera.h"
#include "light.h"

using namespace std;

class Scene {
public:
	Scene(Context *ctx, const bool lowLatency, const string &fileName, Film *film);
	~Scene() {
		delete camera;
		delete[] lights;
		delete dataSet;
		mesh->Delete();
		delete mesh;
	}

	unsigned int SampleLights(const float u) const {
		// One Uniform light strategy
		const unsigned int lightIndex = min(Floor2UInt(nLights * u), nLights - 1);

		return lightIndex;
	}

	bool IsLight(const unsigned int index) const {
		return (index >= meshLightOffset);
	}

	// Siggned because of the delta parameter
	int maxPathDepth;
	unsigned int shadowRayCount;

	PerspectiveCamera *camera;

	unsigned int nLights;
	unsigned int meshLightOffset;
	ExtTriangleMesh *mesh;
	TriangleLight *lights;
	DataSet *dataSet;
};

#endif	/* _SCENE_H */