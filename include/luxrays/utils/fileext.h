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

#ifndef _LUXRAYS_FILEEXT_H
#define	_LUXRAYS_FILEEXT_H

#include <string>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/case_conv.hpp>

namespace slg {

inline std::string GetFileNameExt(const std::string &fileName) {
	return boost::algorithm::to_lower_copy(boost::filesystem::path(fileName).extension().string());
}

inline std::string GetFileNamePath(const std::string &fileName) {
	boost::filesystem::path path(fileName);

	if (path.has_parent_path())
		return boost::filesystem::path(fileName).parent_path().string() + "/";
	else
		return "";
}

}

#endif	/* _LUXRAYS_FILEEXT_H */
