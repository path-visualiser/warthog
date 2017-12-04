#include "contraction.h"
#include "fch_down_dfs_expansion_policy.h"
#include "fch_expansion_policy.h"
#include "planar_graph.h"
#include "search_node.h"
#include "zero_heuristic.h"
#include "flexible_astar.h"
#include "problem_instance.h"
#include "solution.h"

warthog::fch_down_dfs_expansion_policy::fch_down_dfs_expansion_policy(
        warthog::graph::planar_graph* g, std::vector<uint32_t>* rank)
    : expansion_policy(g->get_num_nodes()), g_(g) 
{
    rank_ = rank;

    edge_labels_ = new std::vector< std::vector< fch_interval >> ();
    node_labels_ = new std::vector<int32_t>();


    t_label = s_label = INT32_MAX;

    // sort edges ("up" before "down") and remember for each node 
    // how many "up" edges there are
    down_heads_ = new uint8_t[g->get_num_nodes()];
    warthog::ch::fch_sort_successors(g, rank, down_heads_);

    // compute down-edge labels
    compute_down_dijkstra_postorder();

}

warthog::fch_down_dfs_expansion_policy::~fch_down_dfs_expansion_policy()
{
    delete edge_labels_;
    delete node_labels_;
    delete [] down_heads_;
}

void
warthog::fch_down_dfs_expansion_policy::expand(
        warthog::search_node* current, warthog::problem_instance*)
{
    reset();

    warthog::search_node* pn = current->get_parent();
    uint32_t current_id = current->get_id();
    uint32_t current_rank = get_rank(current_id);

    warthog::graph::node* n = g_->get_node(current_id);
    warthog::graph::edge_iter begin, end;
    begin = n->outgoing_begin();
    end = n->outgoing_end();

    // traveling up the hierarchy we generate all neighbours;
    // traveling down, we generate only "down" neighbours
    bool up_travel = !pn || (current_rank > get_rank(pn->get_id()));
    if(up_travel) 
    { 
        for(warthog::graph::edge_iter it = begin; it != end; it++)
        {
            warthog::graph::edge& e = *it;
            assert(e.node_id_ < g_->get_num_nodes());
            this->add_neighbour(this->generate(e.node_id_), e.wt_);
        }
    }
    else 
    {
        begin += down_heads_[current_id];
        for(warthog::graph::edge_iter it = begin; it != end; it++)
        {
            warthog::graph::edge& e = *it;
            assert(e.node_id_ < g_->get_num_nodes());
            if(!filter(current_id, (it - begin)))
            {
                this->add_neighbour(this->generate(e.node_id_), e.wt_);
            }
        }
    }
}

void
warthog::fch_down_dfs_expansion_policy::get_xy(uint32_t nid, int32_t& x, int32_t& y)
{
    g_->get_xy(nid, x, y);
}

warthog::search_node* 
warthog::fch_down_dfs_expansion_policy::generate_start_node(
        warthog::problem_instance* pi)
{
    uint32_t s_graph_id = g_->to_graph_id(pi->start_id_);
    if(s_graph_id == warthog::INF) { return 0; }
    s_label = node_labels_->at(s_graph_id);
    return generate(s_graph_id);
}

warthog::search_node*
warthog::fch_down_dfs_expansion_policy::generate_target_node(
        warthog::problem_instance* pi)
{
    uint32_t t_graph_id = g_->to_graph_id(pi->target_id_);
    if(t_graph_id == warthog::INF) { return 0; }
    t_label = node_labels_->at(t_graph_id);
    return generate(t_graph_id);
}

void
warthog::fch_down_dfs_expansion_policy::compute_down_dijkstra_postorder()
{
    // allocate data for first move pre-computation
    const int FM_BYTES = 32;
    struct fm_label
    {
        fm_label() 
        { for(uint32_t i = 0; i < FM_BYTES; i++) { lab_[i] = 0; } }

        fm_label& 
        operator=(fm_label& other)
        { 
            for(uint32_t i = 0; i < FM_BYTES; i++) { lab_[i] = other.lab_[i];}
            return *this;
        }

        void
        add(uint32_t move_id)
        {
            uint32_t fm_byte = move_id / 8;
            uint32_t fm_bit = move_id % 8;
            lab_[fm_byte] |= (1 << fm_bit);
        }

        void
        cup(fm_label& other)
        { for(uint32_t i = 0; i < FM_BYTES; i++) { lab_[i] |= other.lab_[i];}}

        bool
        intersect(fm_label& other)
        {
            uint8_t retval = 0;
            for(uint32_t i = 0; i < FM_BYTES; i++) 
            { 
                retval |= (lab_[i] & other.lab_[i]);
            }
            return retval;
        }

        uint8_t lab_[FM_BYTES];
    };

    // identify the apex and allocate memory for (node and down-edge) labels
    node_labels_->clear();
    node_labels_->resize(g_->get_num_nodes(), INT32_MAX);
    edge_labels_->clear();
    edge_labels_->resize(g_->get_num_nodes());
    uint32_t internal_source_id = 0;
    for(uint32_t cur_id = 0; cur_id < rank_->size(); cur_id++)
    { 
        if(get_rank(cur_id) > get_rank(internal_source_id)) 
        { internal_source_id = cur_id; } 

        warthog::graph::node* cur = g_->get_node(cur_id);
        
        uint32_t num_down = cur->outgoing_end() - 
            (cur->outgoing_begin() + down_heads_[cur_id]);

        edge_labels_->at(cur_id).resize( num_down );
        assert(edge_labels_->at(cur_id).size() == num_down);
    }

    // alocate memory for the first moves
    std::vector<fm_label> first_move(g_->get_num_nodes());

    // callback function used to record the optimal first move 
    std::function<void(warthog::search_node*, warthog::search_node*,
            double, uint32_t)> on_generate_fn = 
    [&internal_source_id, &first_move]
    (warthog::search_node* succ, warthog::search_node* from,
                double edge_cost, uint32_t edge_id) -> void
    {
        uint32_t s_id = succ->get_id();
        uint32_t f_id = from->get_id();

        if(from->get_id() == internal_source_id) // start node successors 
        { first_move.at(s_id).add(edge_id); }
        else // all other nodes
        {
            double alt_g = from->get_g() + edge_cost;
            double g_val = succ->get_search_id() == from->get_search_id() 
                                ? succ->get_g() : warthog::INF; 
            //  update first move
            if(alt_g < g_val) 
            { first_move.at(s_id) = first_move.at(f_id); }

            // add alternative first-move
            if(alt_g == g_val)
            { first_move.at(s_id).cup(first_move.at(f_id)); }
        }
    };
   
    // dijkstra search from the apex node
    warthog::zero_heuristic h;
    warthog::fch_expansion_policy exp(g_, rank_);
    warthog::flexible_astar 
        <warthog::zero_heuristic, warthog::fch_expansion_policy>
            dijk(&h, &exp);
    dijk.apply_on_generate(on_generate_fn);
    uint32_t ext_source_id = g_->to_external_id(internal_source_id);
    warthog::problem_instance problem(ext_source_id, warthog::INF);
    //problem.verbose_ = true;
    warthog::solution sol;
    dijk.get_path(problem, sol);

    // traverse the graph and compute node and edge labels using DFS postorder
    fch_interval wtf;
    std::vector<fch_interval> node_range(g_->get_num_nodes());
    uint32_t next_label = 0;
    std::function<fch_interval(uint32_t)> label_fn = 
        [&wtf, this, &node_range, &internal_source_id, &first_move, 
        &next_label, &label_fn] (uint32_t current_id) 
        -> fch_interval
        {
            fch_interval foo;
            assert(wtf.left == foo.left && wtf.right == foo.right);

            fch_interval dfs_range;
            warthog::graph::node* source = this->g_->get_node(current_id);
            fm_label move = first_move.at(current_id);

            warthog::graph::edge_iter begin = 
                source->outgoing_begin() + down_heads_[current_id];
            for( warthog::graph::edge_iter it = begin; 
                    it != source->outgoing_end();
                    it++)
            {
                // init edge label
                uint32_t edge_idx = it - begin;
                assert(edge_idx < edge_labels_->at(current_id).size());
                
                // DFS
                if(first_move.at(it->node_id_).intersect(move))
                {
                    fch_interval subtree_range = node_range.at((*it).node_id_);
                    if(subtree_range.left == INT32_MAX)
                    {
                        subtree_range = label_fn(it->node_id_);
                        node_range.at(it->node_id_) = subtree_range;
                    }

                    edge_labels_->at(current_id).at(edge_idx) = 
                        subtree_range;
                    dfs_range.merge(subtree_range);

                    assert(dfs_range.contains(node_labels_->at(it->node_id_)));
                }
            }
            if(this->node_labels_->at(current_id) == INT32_MAX)
            {
                this->node_labels_->at(current_id) = next_label++;
            }
            dfs_range.grow(node_labels_->at(current_id));
            return dfs_range;
        };

    for(uint32_t i = 0; i < FM_BYTES; i++)
    {
        first_move.at(internal_source_id).lab_[i] = 255; 
    }
    label_fn(internal_source_id);
}

