/*

Copyright (c) 2013, Project OSRM, Dennis Luxen, others
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list
of conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright notice, this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef NEAREST_PLUGIN_H
#define NEAREST_PLUGIN_H

#include "BasePlugin.h"
#include "../DataStructures/JSONContainer.h"
#include "../DataStructures/PhantomNodes.h"
#include "../Util/StringUtil.h"

#include <string>

/*
 * This Plugin locates the nearest point on a street in the road network for a given coordinate.
 */

template <class DataFacadeT> class NearestPlugin : public BasePlugin
{
  public:
    explicit NearestPlugin(DataFacadeT *facade) : facade(facade), descriptor_string("nearest")
    {
    }

    const std::string GetDescriptor() const { return descriptor_string; }

    void HandleRequest(const RouteParameters &route_parameters, http::Reply &reply)
    {
        // check number of parameters
        if (route_parameters.coordinates.empty() ||
            !route_parameters.coordinates.front().isValid())
        {
            reply = http::Reply::StockReply(http::Reply::badRequest);
            return;
        }

        PhantomNode phantom_node;
        facade->FindPhantomNodeForCoordinate(
            route_parameters.coordinates.front(), phantom_node, route_parameters.zoom_level);

        JSON::Object json_result;
        if (!phantom_node.isValid())
        {
            json_result.values["status"] = 207;
        }
        else
        {
            reply.status = http::Reply::ok;
            json_result.values["status"] = 0;
                        JSON::Array json_coordinate;
            json_coordinate.values.push_back(phantom_node.location.lat/COORDINATE_PRECISION);
            json_coordinate.values.push_back(phantom_node.location.lon/COORDINATE_PRECISION);
            json_result.values["mapped_coordinate"] = json_coordinate;
            std::string temp_string;
            facade->GetName(phantom_node.name_id, temp_string);
            json_result.values["name"] = temp_string;
        }

        JSON::render(reply.content, json_result);
    }

  private:
    DataFacadeT *facade;
    std::string descriptor_string;
};

#endif /* NEAREST_PLUGIN_H */
