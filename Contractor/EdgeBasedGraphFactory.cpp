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

#include "EdgeBasedGraphFactory.h"
#include "../Algorithms/BFSComponentExplorer.h"
#include "../DataStructures/Percent.h"
#include "../Util/ComputeAngle.h"
#include "../Util/LuaUtil.h"
#include "../Util/SimpleLogger.h"
#include "../Util/TimingUtil.h"

#include <boost/assert.hpp>

#include <fstream>
#include <limits>

EdgeBasedGraphFactory::EdgeBasedGraphFactory(
    const std::shared_ptr<NodeBasedDynamicGraph> &node_based_graph,
    std::unique_ptr<RestrictionMap> restriction_map,
    std::vector<NodeID> &barrier_node_list,
    std::vector<NodeID> &traffic_light_node_list,
    std::vector<NodeInfo> &m_node_info_list,
    SpeedProfileProperties &speed_profile)
    : speed_profile(speed_profile),
      m_number_of_edge_based_nodes(std::numeric_limits<unsigned>::max()),
      m_node_info_list(m_node_info_list), m_node_based_graph(node_based_graph),
      m_restriction_map(std::move(restriction_map)), max_id(0)
{

    // insert into unordered sets for fast lookup
    m_barrier_nodes.insert(barrier_node_list.begin(), barrier_node_list.end());
    m_traffic_lights.insert(traffic_light_node_list.begin(), traffic_light_node_list.end());
}

void EdgeBasedGraphFactory::GetEdgeBasedEdges(DeallocatingVector<EdgeBasedEdge> &output_edge_list)
{
    BOOST_ASSERT_MSG(0 == output_edge_list.size(), "Vector is not empty");
    m_edge_based_edge_list.swap(output_edge_list);
}

void EdgeBasedGraphFactory::GetEdgeBasedNodes(std::vector<EdgeBasedNode> &nodes)
{
#ifndef NDEBUG
    for (const EdgeBasedNode &node : m_edge_based_node_list)
    {
        BOOST_ASSERT(m_node_info_list.at(node.u).lat != INT_MAX);
        BOOST_ASSERT(m_node_info_list.at(node.u).lon != INT_MAX);
        BOOST_ASSERT(m_node_info_list.at(node.v).lon != INT_MAX);
        BOOST_ASSERT(m_node_info_list.at(node.v).lat != INT_MAX);
    }
#endif
    nodes.swap(m_edge_based_node_list);
}

void
EdgeBasedGraphFactory::InsertEdgeBasedNode(NodeID u, NodeID v, EdgeID e1, bool belongs_to_tiny_cc)
{
    // merge edges together into one EdgeBasedNode
    BOOST_ASSERT(u != SPECIAL_NODEID);
    BOOST_ASSERT(v != SPECIAL_NODEID);
    BOOST_ASSERT(e1 != SPECIAL_EDGEID);

#ifndef NDEBUG
    // find forward edge id and
    const EdgeID e1b = m_node_based_graph->FindEdge(u, v);
    BOOST_ASSERT(e1 == e1b);
#endif

    BOOST_ASSERT(e1 != SPECIAL_EDGEID);
    const EdgeData &forward_data = m_node_based_graph->GetEdgeData(e1);

    // find reverse edge id and
    const EdgeID e2 = m_node_based_graph->FindEdge(v, u);

#ifndef NDEBUG
    if (e2 == m_node_based_graph->EndEdges(v))
    {
        SimpleLogger().Write(logWARNING) << "Did not find edge (" << v << "," << u << ")";
    }
#endif
    BOOST_ASSERT(e2 != SPECIAL_EDGEID);
    BOOST_ASSERT(e2 < m_node_based_graph->EndEdges(v));
    const EdgeData &reverse_data = m_node_based_graph->GetEdgeData(e2);

    if (forward_data.edgeBasedNodeID == SPECIAL_NODEID &&
        reverse_data.edgeBasedNodeID == SPECIAL_NODEID)
    {
        return;
    }

    BOOST_ASSERT(m_geometry_compressor.HasEntryForID(e1) ==
                 m_geometry_compressor.HasEntryForID(e2));
    if (m_geometry_compressor.HasEntryForID(e1))
    {
        BOOST_ASSERT(m_geometry_compressor.HasEntryForID(e2));

        // reconstruct geometry and put in each individual edge with its offset
        const std::vector<GeometryCompressor::CompressedNode> &forward_geometry =
            m_geometry_compressor.GetBucketReference(e1);
        const std::vector<GeometryCompressor::CompressedNode> &reverse_geometry =
            m_geometry_compressor.GetBucketReference(e2);
        BOOST_ASSERT(forward_geometry.size() == reverse_geometry.size());
        BOOST_ASSERT(0 != forward_geometry.size());

        // reconstruct bidirectional edge with individual weights and put each into the NN index

        std::vector<int> forward_dist_prefix_sum(forward_geometry.size(), 0);
        std::vector<int> reverse_dist_prefix_sum(reverse_geometry.size(), 0);

        // quick'n'dirty prefix sum as std::partial_sum needs addtional casts
        // TODO: move to lambda function with C++11
        int temp_sum = 0;

        for (unsigned i = 0; i < forward_geometry.size(); ++i)
        {
            forward_dist_prefix_sum[i] = temp_sum;
            temp_sum += forward_geometry[i].second;

            BOOST_ASSERT(forward_data.distance >= temp_sum);
        }

        temp_sum = 0;
        for (unsigned i = 0; i < reverse_geometry.size(); ++i)
        {
            temp_sum += reverse_geometry[reverse_geometry.size() - 1 - i].second;
            reverse_dist_prefix_sum[i] = reverse_data.distance - temp_sum;
            BOOST_ASSERT(reverse_data.distance >= temp_sum);
        }

        BOOST_ASSERT(forward_geometry.size() == reverse_geometry.size());

        const unsigned geometry_size = forward_geometry.size();
        BOOST_ASSERT(geometry_size > 1);
        NodeID current_edge_start_coordinate_id = u;

        if (forward_data.edgeBasedNodeID != SPECIAL_NODEID)
        {
            max_id = std::max(forward_data.edgeBasedNodeID, max_id);
        }
        if (SPECIAL_NODEID != reverse_data.edgeBasedNodeID)
        {
            max_id = std::max(reverse_data.edgeBasedNodeID, max_id);
        }

        // traverse arrays from start and end respectively
        for (unsigned i = 0; i < geometry_size; ++i)
        {
            BOOST_ASSERT(current_edge_start_coordinate_id ==
                         reverse_geometry[geometry_size - 1 - i].first);
            const NodeID current_edge_target_coordinate_id = forward_geometry[i].first;
            BOOST_ASSERT(current_edge_target_coordinate_id != current_edge_start_coordinate_id);

            // build edges
            m_edge_based_node_list.emplace_back(forward_data.edgeBasedNodeID,
                                                reverse_data.edgeBasedNodeID,
                                                current_edge_start_coordinate_id,
                                                current_edge_target_coordinate_id,
                                                forward_data.nameID,
                                                forward_geometry[i].second,
                                                reverse_geometry[i].second,
                                                forward_dist_prefix_sum[i],
                                                reverse_dist_prefix_sum[i],
                                                m_geometry_compressor.GetPositionForID(e1),
                                                i,
                                                belongs_to_tiny_cc);
            current_edge_start_coordinate_id = current_edge_target_coordinate_id;

            BOOST_ASSERT(m_edge_based_node_list.back().IsCompressed());

            BOOST_ASSERT(u != m_edge_based_node_list.back().u ||
                         v != m_edge_based_node_list.back().v);

            BOOST_ASSERT(u != m_edge_based_node_list.back().v ||
                         v != m_edge_based_node_list.back().u);
        }

        BOOST_ASSERT(current_edge_start_coordinate_id == v);
        BOOST_ASSERT(m_edge_based_node_list.back().IsCompressed());
    }
    else
    {
        BOOST_ASSERT(!m_geometry_compressor.HasEntryForID(e2));

        if (forward_data.edgeBasedNodeID != SPECIAL_NODEID)
        {
            BOOST_ASSERT(forward_data.forward);
        }
        if (reverse_data.edgeBasedNodeID != SPECIAL_NODEID)
        {
            BOOST_ASSERT(reverse_data.forward);
        }
        if (forward_data.edgeBasedNodeID == SPECIAL_NODEID)
        {
            BOOST_ASSERT(!forward_data.forward);
        }
        if (reverse_data.edgeBasedNodeID == SPECIAL_NODEID)
        {
            BOOST_ASSERT(!reverse_data.forward);
        }

        BOOST_ASSERT(forward_data.edgeBasedNodeID != SPECIAL_NODEID ||
                     reverse_data.edgeBasedNodeID != SPECIAL_NODEID);

        m_edge_based_node_list.emplace_back(EdgeBasedNode(forward_data.edgeBasedNodeID,
                                                          reverse_data.edgeBasedNodeID,
                                                          u,
                                                          v,
                                                          forward_data.nameID,
                                                          forward_data.distance,
                                                          reverse_data.distance,
                                                          0,
                                                          0,
                                                          SPECIAL_EDGEID,
                                                          0,
                                                          belongs_to_tiny_cc));
        BOOST_ASSERT(!m_edge_based_node_list.back().IsCompressed());
    }
}

void EdgeBasedGraphFactory::FlushVectorToStream(
    std::ofstream &edge_data_file, std::vector<OriginalEdgeData> &original_edge_data_vector) const
{
    edge_data_file.write((char *)&(original_edge_data_vector[0]),
                         original_edge_data_vector.size() * sizeof(OriginalEdgeData));
    original_edge_data_vector.clear();
}

void EdgeBasedGraphFactory::Run(const std::string &original_edge_data_filename,
                                const std::string &geometry_filename,
                                lua_State *lua_state)
{

    TIMER_START(geometry);
    CompressGeometry();
    TIMER_STOP(geometry);

    TIMER_START(renumber);
    RenumberEdges();
    TIMER_STOP(renumber);

    TIMER_START(generate_nodes);
    GenerateEdgeExpandedNodes();
    TIMER_STOP(generate_nodes);

    TIMER_START(generate_edges);
    GenerateEdgeExpandedEdges(original_edge_data_filename, lua_state);
    TIMER_STOP(generate_edges);

    m_geometry_compressor.SerializeInternalVector(geometry_filename);

    SimpleLogger().Write() << "Timing statistics for edge-expanded graph:";
    SimpleLogger().Write() << "Geometry compression: " << TIMER_MSEC(geometry)*0.001 << "s";
    SimpleLogger().Write() << "Renumbering edges: " << TIMER_MSEC(renumber)*0.001 << "s";
    SimpleLogger().Write() << "Generating nodes: " << TIMER_MSEC(generate_nodes)*0.001 << "s";
    SimpleLogger().Write() << "Generating edges: " << TIMER_MSEC(generate_edges)*0.001 << "s";
}

void EdgeBasedGraphFactory::CompressGeometry()
{
    SimpleLogger().Write() << "Removing graph geometry while preserving topology";

    const unsigned original_number_of_nodes = m_node_based_graph->GetNumberOfNodes();
    const unsigned original_number_of_edges = m_node_based_graph->GetNumberOfEdges();

    Percent p(original_number_of_nodes);
    unsigned removed_node_count = 0;

    for (NodeID v = 0; v < original_number_of_nodes; ++v)
    {
        p.printStatus(v);

        // only contract degree 2 vertices
        if (2 != m_node_based_graph->GetOutDegree(v))
        {
            continue;
        }

        // don't contract barrier node
        if (m_barrier_nodes.end() != m_barrier_nodes.find(v))
        {
            continue;
        }

        const bool reverse_edge_order =
            !(m_node_based_graph->GetEdgeData(m_node_based_graph->BeginEdges(v)).forward);
        const EdgeID forward_e2 = m_node_based_graph->BeginEdges(v) + reverse_edge_order;
        BOOST_ASSERT(SPECIAL_EDGEID != forward_e2);
        const EdgeID reverse_e2 = m_node_based_graph->BeginEdges(v) + 1 - reverse_edge_order;
        BOOST_ASSERT(SPECIAL_EDGEID != reverse_e2);

        const EdgeData &fwd_edge_data2 = m_node_based_graph->GetEdgeData(forward_e2);
        const EdgeData &rev_edge_data2 = m_node_based_graph->GetEdgeData(reverse_e2);

        const NodeID w = m_node_based_graph->GetTarget(forward_e2);
        BOOST_ASSERT(SPECIAL_NODEID != w);
        BOOST_ASSERT(v != w);
        const NodeID u = m_node_based_graph->GetTarget(reverse_e2);
        BOOST_ASSERT(SPECIAL_NODEID != u);
        BOOST_ASSERT(u != v);

        const EdgeID forward_e1 = m_node_based_graph->FindEdge(u, v);
        BOOST_ASSERT(m_node_based_graph->EndEdges(u) != forward_e1);
        BOOST_ASSERT(SPECIAL_EDGEID != forward_e1);
        BOOST_ASSERT(v == m_node_based_graph->GetTarget(forward_e1));
        const EdgeID reverse_e1 = m_node_based_graph->FindEdge(w, v);
        BOOST_ASSERT(SPECIAL_EDGEID != reverse_e1);
        BOOST_ASSERT(v == m_node_based_graph->GetTarget(reverse_e1));

        const EdgeData &fwd_edge_data1 = m_node_based_graph->GetEdgeData(forward_e1);
        const EdgeData &rev_edge_data1 = m_node_based_graph->GetEdgeData(reverse_e1);

        if ((m_node_based_graph->FindEdge(u, w) != m_node_based_graph->EndEdges(u)) ||
            (m_node_based_graph->FindEdge(w, u) != m_node_based_graph->EndEdges(w)))
        {
            continue;
        }

        if ( // TODO: rename to IsCompatibleTo
                fwd_edge_data1.IsEqualTo(fwd_edge_data2) &&
                rev_edge_data1.IsEqualTo(rev_edge_data2))
        {
            // Get distances before graph is modified
            const int forward_weight1 = m_node_based_graph->GetEdgeData(forward_e1).distance;
            const int forward_weight2 = m_node_based_graph->GetEdgeData(forward_e2).distance;

            BOOST_ASSERT(0 != forward_weight1);
            BOOST_ASSERT(0 != forward_weight2);

            const int reverse_weight1 = m_node_based_graph->GetEdgeData(reverse_e1).distance;
            const int reverse_weight2 = m_node_based_graph->GetEdgeData(reverse_e2).distance;

            BOOST_ASSERT(0 != reverse_weight1);
            BOOST_ASSERT(0 != forward_weight2);

            const bool add_traffic_signal_penalty =
                (m_traffic_lights.find(v) != m_traffic_lights.end());

            // add weight of e2's to e1
            m_node_based_graph->GetEdgeData(forward_e1).distance += fwd_edge_data2.distance;
            m_node_based_graph->GetEdgeData(reverse_e1).distance += rev_edge_data2.distance;
            if (add_traffic_signal_penalty)
            {
                m_node_based_graph->GetEdgeData(forward_e1).distance +=
                    speed_profile.trafficSignalPenalty;
                m_node_based_graph->GetEdgeData(reverse_e1).distance +=
                    speed_profile.trafficSignalPenalty;
            }

            // extend e1's to targets of e2's
            m_node_based_graph->SetTarget(forward_e1, w);
            m_node_based_graph->SetTarget(reverse_e1, u);

            // remove e2's (if bidir, otherwise only one)
            m_node_based_graph->DeleteEdge(v, forward_e2);
            m_node_based_graph->DeleteEdge(v, reverse_e2);

            // update any involved turn restrictions
            m_restriction_map->FixupStartingTurnRestriction(u, v, w);
            m_restriction_map->FixupArrivingTurnRestriction(u, v, w);

            m_restriction_map->FixupStartingTurnRestriction(w, v, u);
            m_restriction_map->FixupArrivingTurnRestriction(w, v, u);

            // store compressed geometry in container
            m_geometry_compressor.CompressEdge(
                forward_e1,
                forward_e2,
                v,
                w,
                forward_weight1 +
                    (add_traffic_signal_penalty ? speed_profile.trafficSignalPenalty : 0),
                forward_weight2);
            m_geometry_compressor.CompressEdge(
                reverse_e1,
                reverse_e2,
                v,
                u,
                reverse_weight1,
                reverse_weight2 +
                    (add_traffic_signal_penalty ? speed_profile.trafficSignalPenalty : 0));
            ++removed_node_count;

            BOOST_ASSERT(m_node_based_graph->GetEdgeData(forward_e1).nameID ==
                         m_node_based_graph->GetEdgeData(reverse_e1).nameID);
        }
    }
    SimpleLogger().Write() << "removed " << removed_node_count << " nodes";
    m_geometry_compressor.PrintStatistics();

    unsigned new_node_count = 0;
    unsigned new_edge_count = 0;
    for (unsigned i = 0; i < m_node_based_graph->GetNumberOfNodes(); ++i)
    {
        if (m_node_based_graph->GetOutDegree(i) > 0)
        {
            ++new_node_count;
            new_edge_count += (m_node_based_graph->EndEdges(i) - m_node_based_graph->BeginEdges(i));
        }
    }
    SimpleLogger().Write() << "new nodes: " << new_node_count << ", edges " << new_edge_count;
    SimpleLogger().Write() << "Node compression ratio: " << new_node_count /
                                                                (double)original_number_of_nodes;
    SimpleLogger().Write() << "Edge compression ratio: " << new_edge_count /
                                                                (double)original_number_of_edges;
}

/**
 * Writes the id of the edge in the edge expanded graph (into the egde in the node based graph)
 */
void EdgeBasedGraphFactory::RenumberEdges()
{
    // renumber edge based node IDs
    unsigned numbered_edges_count = 0;
    for (NodeID current_node = 0; current_node < m_node_based_graph->GetNumberOfNodes();
         ++current_node)
    {
        for (EdgeID current_edge = m_node_based_graph->BeginEdges(current_node);
             current_edge < m_node_based_graph->EndEdges(current_node);
             ++current_edge)
        {
            EdgeData &edge_data = m_node_based_graph->GetEdgeData(current_edge);
            if (!edge_data.forward)
            {
                continue;
            }

            BOOST_ASSERT(numbered_edges_count < m_node_based_graph->GetNumberOfEdges());
            edge_data.edgeBasedNodeID = numbered_edges_count;
            ++numbered_edges_count;

            BOOST_ASSERT(SPECIAL_NODEID != edge_data.edgeBasedNodeID);
        }
    }
    m_number_of_edge_based_nodes = numbered_edges_count;
}

/**
 * Creates the nodes in the edge expanded graph from edges in the node-based graph.
 */
void EdgeBasedGraphFactory::GenerateEdgeExpandedNodes()
{
    SimpleLogger().Write() << "Identifying components of the road network";

    // Run a BFS on the undirected graph and identify small components
    BFSComponentExplorer<NodeBasedDynamicGraph> component_explorer(
        *m_node_based_graph, *m_restriction_map, m_barrier_nodes);

    component_explorer.run();

    SimpleLogger().Write() << "identified: " << component_explorer.GetNumberOfComponents()
                           << " many components";
    SimpleLogger().Write() << "generating edge-expanded nodes";

    Percent p(m_node_based_graph->GetNumberOfNodes());

    // loop over all edges and generate new set of nodes
    for (NodeID u = 0, end = m_node_based_graph->GetNumberOfNodes(); u < end; ++u)
    {
        BOOST_ASSERT(u != SPECIAL_NODEID);
        BOOST_ASSERT(u < m_node_based_graph->GetNumberOfNodes());
        p.printIncrement();
        for (EdgeID e1 = m_node_based_graph->BeginEdges(u),
                    last_edge = m_node_based_graph->EndEdges(u);
             e1 < last_edge;
             ++e1)
        {
            const EdgeData &edge_data = m_node_based_graph->GetEdgeData(e1);
            if (edge_data.edgeBasedNodeID == SPECIAL_NODEID)
            {
                // continue;
            }
            BOOST_ASSERT(e1 != SPECIAL_EDGEID);
            const NodeID v = m_node_based_graph->GetTarget(e1);

            BOOST_ASSERT(SPECIAL_NODEID != v);
            // pick only every other edge
            if (u > v)
            {
                continue;
            }

            BOOST_ASSERT(u < v);
            BOOST_ASSERT(edge_data.type != SHRT_MAX);

            // Note: edges that end on barrier nodes or on a turn restriction
            // may actually be in two distinct components. We choose the smallest
            const unsigned size_of_component = std::min(component_explorer.GetComponentSize(u),
                                                        component_explorer.GetComponentSize(v));

            const bool component_is_tiny = (size_of_component < 1000);
            InsertEdgeBasedNode(u, v, e1, component_is_tiny);
        }
    }

    SimpleLogger().Write() << "Generated " << m_edge_based_node_list.size()
                           << " nodes in edge-expanded graph";
}

/**
 * Actually it also generates OriginalEdgeData and serializes them...
 */
void
EdgeBasedGraphFactory::GenerateEdgeExpandedEdges(const std::string &original_edge_data_filename,
                                                 lua_State *lua_state)
{
    SimpleLogger().Write() << "generating edge-expanded edges";

    unsigned node_based_edge_counter = 0;
    unsigned original_edges_counter = 0;

    std::ofstream edge_data_file(original_edge_data_filename.c_str(), std::ios::binary);

    // writes a dummy value that is updated later
    edge_data_file.write((char *)&original_edges_counter, sizeof(unsigned));

    std::vector<OriginalEdgeData> original_edge_data_vector;
    original_edge_data_vector.reserve(1024 * 1024);

    // Loop over all turns and generate new set of edges.
    // Three nested loop look super-linear, but we are dealing with a (kind of)
    // linear number of turns only.
    unsigned restricted_turns_counter = 0;
    unsigned skipped_uturns_counter = 0;
    unsigned skipped_barrier_turns_counter = 0;
    unsigned compressed = 0;

    Percent p(m_node_based_graph->GetNumberOfNodes());

    for (NodeID u = 0, end = m_node_based_graph->GetNumberOfNodes(); u < end; ++u)
    {
        for (EdgeID e1 = m_node_based_graph->BeginEdges(u),
                    last_edge_u = m_node_based_graph->EndEdges(u);
             e1 < last_edge_u;
             ++e1)
        {
            if (!m_node_based_graph->GetEdgeData(e1).forward)
            {
                continue;
            }

            ++node_based_edge_counter;
            const NodeID v = m_node_based_graph->GetTarget(e1);
            const NodeID to_node_of_only_restriction =
                m_restriction_map->CheckForEmanatingIsOnlyTurn(u, v);
            const bool is_barrier_node = (m_barrier_nodes.find(v) != m_barrier_nodes.end());

            for (EdgeID e2 = m_node_based_graph->BeginEdges(v),
                        last_edge_v = m_node_based_graph->EndEdges(v);
                 e2 < last_edge_v;
                 ++e2)
            {
                if (!m_node_based_graph->GetEdgeData(e2).forward)
                {
                    continue;
                }
                const NodeID w = m_node_based_graph->GetTarget(e2);

                if ((to_node_of_only_restriction != SPECIAL_NODEID) &&
                    (w != to_node_of_only_restriction))
                {
                    // We are at an only_-restriction but not at the right turn.
                    ++restricted_turns_counter;
                    continue;
                }

                if (is_barrier_node)
                {
                    if (u != w)
                    {
                        ++skipped_barrier_turns_counter;
                        continue;
                    }
                }
                else
                {
                    if ((u == w) && (m_node_based_graph->GetOutDegree(v) > 1))
                    {
                        ++skipped_uturns_counter;
                        continue;
                    }
                }

                // only add an edge if turn is not a U-turn except when it is
                // at the end of a dead-end street
                if (m_restriction_map->CheckIfTurnIsRestricted(u, v, w) &&
                    (to_node_of_only_restriction == SPECIAL_NODEID) &&
                    (w != to_node_of_only_restriction))
                {
                    ++restricted_turns_counter;
                    continue;
                }

                // only add an edge if turn is not prohibited
                const EdgeData &edge_data1 = m_node_based_graph->GetEdgeData(e1);
                const EdgeData &edge_data2 = m_node_based_graph->GetEdgeData(e2);

                BOOST_ASSERT(edge_data1.edgeBasedNodeID != edge_data2.edgeBasedNodeID);
                BOOST_ASSERT(edge_data1.forward);
                BOOST_ASSERT(edge_data2.forward);

                // the following is the core of the loop.
                unsigned distance = edge_data1.distance;
                if (m_traffic_lights.find(v) != m_traffic_lights.end())
                {
                    distance += speed_profile.trafficSignalPenalty;
                }
                const int turn_penalty = GetTurnPenalty(u, v, w, lua_state);
                TurnInstruction turn_instruction = AnalyzeTurn(u, v, w);
                if (turn_instruction == TurnInstruction::UTurn)
                {
                    distance += speed_profile.uTurnPenalty;
                }
                distance += turn_penalty;

                const bool edge_is_compressed = m_geometry_compressor.HasEntryForID(e1);

                if (edge_is_compressed)
                {
                    ++compressed;
                }

                original_edge_data_vector.emplace_back(
                    (edge_is_compressed ? m_geometry_compressor.GetPositionForID(e1) : v),
                    edge_data1.nameID,
                    turn_instruction,
                    edge_is_compressed);

                ++original_edges_counter;

                if (original_edge_data_vector.size() > 1024 * 1024 * 10)
                {
                    FlushVectorToStream(edge_data_file, original_edge_data_vector);
                }

                BOOST_ASSERT(SPECIAL_NODEID != edge_data1.edgeBasedNodeID);
                BOOST_ASSERT(SPECIAL_NODEID != edge_data2.edgeBasedNodeID);

                m_edge_based_edge_list.emplace_back(EdgeBasedEdge(edge_data1.edgeBasedNodeID,
                                                                  edge_data2.edgeBasedNodeID,
                                                                  m_edge_based_edge_list.size(),
                                                                  distance,
                                                                  true,
                                                                  false));
            }
        }
        p.printIncrement();
    }
    FlushVectorToStream(edge_data_file, original_edge_data_vector);

    edge_data_file.seekp(std::ios::beg);
    edge_data_file.write((char *)&original_edges_counter, sizeof(unsigned));
    edge_data_file.close();

    SimpleLogger().Write() << "Generated " << m_edge_based_node_list.size() << " edge based nodes";
    SimpleLogger().Write() << "Node-based graph contains " << node_based_edge_counter << " edges";
    SimpleLogger().Write() << "Edge-expanded graph ...";
    SimpleLogger().Write() << "  contains " << m_edge_based_edge_list.size() << " edges";
    SimpleLogger().Write() << "  skips " << restricted_turns_counter << " turns, "
                                                                        "defined by "
                           << m_restriction_map->size() << " restrictions";
    SimpleLogger().Write() << "  skips " << skipped_uturns_counter << " U turns";
    SimpleLogger().Write() << "  skips " << skipped_barrier_turns_counter << " turns over barriers";
}

int EdgeBasedGraphFactory::GetTurnPenalty(const NodeID u,
                                          const NodeID v,
                                          const NodeID w,
                                          lua_State *lua_state) const
{
    const double angle = GetAngleBetweenThreeFixedPointCoordinates(
        m_node_info_list[u], m_node_info_list[v], m_node_info_list[w]);

    if (speed_profile.has_turn_penalty_function)
    {
        try
        {
            // call lua profile to compute turn penalty
            return luabind::call_function<int>(lua_state, "turn_function", 180. - angle);
        }
        catch (const luabind::error &er) { SimpleLogger().Write(logWARNING) << er.what(); }
    }
    return 0;
}

TurnInstruction EdgeBasedGraphFactory::AnalyzeTurn(const NodeID u, const NodeID v, const NodeID w)
    const
{
    if (u == w)
    {
        return TurnInstruction::UTurn;
    }

    const EdgeID edge1 = m_node_based_graph->FindEdge(u, v);
    const EdgeID edge2 = m_node_based_graph->FindEdge(v, w);

    const EdgeData &data1 = m_node_based_graph->GetEdgeData(edge1);
    const EdgeData &data2 = m_node_based_graph->GetEdgeData(edge2);

    if (!data1.contraFlow && data2.contraFlow)
    {
        return TurnInstruction::EnterAgainstAllowedDirection;
    }
    if (data1.contraFlow && !data2.contraFlow)
    {
        return TurnInstruction::LeaveAgainstAllowedDirection;
    }

    // roundabouts need to be handled explicitely
    if (data1.roundabout && data2.roundabout)
    {
        // Is a turn possible? If yes, we stay on the roundabout!
        if (1 == m_node_based_graph->GetOutDegree(v))
        {
            // No turn possible.
            return TurnInstruction::NoTurn;
        }
        return TurnInstruction::StayOnRoundAbout;
    }
    // Does turn start or end on roundabout?
    if (data1.roundabout || data2.roundabout)
    {
        // We are entering the roundabout
        if ((!data1.roundabout) && data2.roundabout)
        {
            return TurnInstruction::EnterRoundAbout;
        }
        // We are leaving the roundabout
        if (data1.roundabout && (!data2.roundabout))
        {
            return TurnInstruction::LeaveRoundAbout;
        }
    }

    // If street names stay the same and if we are certain that it is not a
    // a segment of a roundabout, we skip it.
    if (data1.nameID == data2.nameID)
    {
        // TODO: Here we should also do a small graph exploration to check for
        //      more complex situations
        if (0 != data1.nameID)
        {
            return TurnInstruction::NoTurn;
        }
        else if (m_node_based_graph->GetOutDegree(v) <= 2)
        {
            return TurnInstruction::NoTurn;
        }
    }

    const double angle = GetAngleBetweenThreeFixedPointCoordinates(
        m_node_info_list[u], m_node_info_list[v], m_node_info_list[w]);
    return TurnInstructionsClass::GetTurnDirectionOfInstruction(angle);
}

unsigned EdgeBasedGraphFactory::GetNumberOfEdgeBasedNodes() const
{
    return m_number_of_edge_based_nodes;
}
