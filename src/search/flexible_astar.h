#ifndef FLEXIBLE_ASTAR_H
#define FLEXIBLE_ASTAR_H

// flexible_astar.h
//
// A* implementation that allows arbitrary combinations of 
// (weighted) heuristic functions and node expansion policies.
// This implementation uses a binary heap for the open_ list
// and a bit array for the closed_ list.
//
// TODO: is it better to store a separate closed list and ungenerate nodes
// or use more memory and not ungenerate until the end of search??
// 32bytes vs... whatever unordered_map overhead is a two integer key/value pair
// 
// @author: dharabor
// @created: 21/08/2012
//

#include "cpool.h"
#include "dummy_filter.h"
#include "pqueue.h"
#include "problem_instance.h"
#include "search.h"
#include "search_node.h"
#include "timer.h"

#include <functional>
#include <iostream>
#include <memory>
#include <stack>
#include <vector>

namespace warthog
{

// H is a heuristic function
// E is an expansion policy
// F is a node filtering (== pruning) policy
template< class H, 
          class E, 
          class F = warthog::dummy_filter>
class flexible_astar : public warthog::search
{
	public:
		flexible_astar(H* heuristic, E* expander, F* filter = 0) :
            heuristic_(heuristic), expander_(expander), filter_(filter)
		{
			open_ = new warthog::pqueue(1024, true);
			verbose_ = false;
            cost_cutoff_ = warthog::INF;
            exp_cutoff_ = warthog::INF;
            on_relax_fn_ = [](warthog::search_node*){ };
            on_norelax_fn_ = 
                [](warthog::search_node*, warthog::search_node*, double){ };
		}

		virtual ~flexible_astar()
		{
			cleanup();
			delete open_;
		}

		inline std::stack<uint32_t>
		get_path(warthog::problem_instance pi)
		{
			warthog::search_node* target = search(pi);
			std::stack<uint32_t> path;
			if(target)
			{
				// follow backpointers to extract the path
				assert(target->get_id() == pi.get_target_id());
				for(warthog::search_node* cur = target;
						cur != 0;
					    cur = cur->get_parent())
                {
					path.push(cur->get_id());
				}
				assert(path.top() == pi.get_start_id());
			}
			//cleanup();
			return path;
		}
        
        // return a list of the nodes expanded during the last search
        // @param coll: an empty list
        void
        closed_list(std::vector<warthog::search_node*>& coll)
        {
            for(uint32_t i = 0; i < expander_->get_nodes_pool_size(); i++)
            {
                warthog::search_node* current = expander_->get_ptr(i, searchid_);
                if(current) { coll.push_back(current); }
            }
        }

        // apply @param fn to every node on the closed list
        void
        apply_to_closed(std::function<void(warthog::search_node*)>& fn)
        {
            for(uint32_t i = 0; i < expander_->get_nodes_pool_size(); i++)
            {
                warthog::search_node* current = 
                    expander_->get_ptr(i, searchid_);
                if(current) { fn(current); }
            }
        }

        // apply @param fn every time a node is successfully relaxed
        void
        apply_on_relax(std::function<void(warthog::search_node*)>& fn)
        {
            on_relax_fn_ = fn;
        }

        // apply @param fn every time a node is reached from a new parent
        // with the same cost as a previous parent
        void
        apply_on_norelax( std::function<void(
                    warthog::search_node* n, 
                    warthog::search_node* current, 
                    double edge_cost)>& fn)
        {
            on_norelax_fn_ = fn;
        }

        // no cleanup after search
		double
		get_length(warthog::problem_instance pi)
		{
			warthog::search_node* target = search(pi);
			double len = warthog::INF;
			if(target)
			{
				len = target->get_g();
			}

            #ifndef NDEBUG
			if(verbose_)
			{
				std::stack<warthog::search_node*> path;
				warthog::search_node* current = target;
				while(current != 0)	
				{
					path.push(current);
					current = current->get_parent();
				}

				while(!path.empty())
				{
					warthog::search_node* n = path.top();
                    int32_t x, y;
                    expander_->get_xy(n->get_id(), x, y);
					std::cerr 
                        << "final path: (" << x << ", " << y << ")...";
					n->print(std::cerr);
					std::cerr << std::endl;
					path.pop();
				}
			}
            #endif
			return len;
		}

        // set a cost-cutoff to run a bounded-cost A* search.
        // the search terminates when the target is found or the f-cost 
        // limit is reached.
        inline void
        set_cost_cutoff(double cutoff) { cost_cutoff_ = cutoff; }

        inline double
        get_cost_cutoff() { return cost_cutoff_; }

        // set a cutoff on the maximum number of node expansions.
        // the search terminates when the target is found or when
        // the limit is reached
        inline void
        set_max_expansions_cutoff(uint32_t cutoff) { exp_cutoff_ = cutoff; }

        inline uint32_t 
        get_max_expansions_cutoff() { return exp_cutoff_; }  

		virtual inline size_t
		mem()
		{
			size_t bytes = 
				// memory for the priority quete
				open_->mem() + 
				// gridmap size and other stuff needed to expand nodes
				expander_->mem() +
                // heuristic uses some memory too
                heuristic_->mem() +
				// misc
				sizeof(*this);
			return bytes;
		}


	private:
		H* heuristic_;
		E* expander_;
        F* filter_;
		warthog::pqueue* open_;

        // early termination limits
        double cost_cutoff_; 
        uint32_t exp_cutoff_;

        // callback for when a node is relaxex
        std::function<void(warthog::search_node*)> on_relax_fn_;

        // callback for when a node is not relaxed
        std::function<void(
                warthog::search_node*, 
                warthog::search_node*, 
                double edge_cost)> on_norelax_fn_;

		// no copy ctor
		flexible_astar(const flexible_astar& other) { } 
		flexible_astar& 
		operator=(const flexible_astar& other) { return *this; }

		warthog::search_node*
		search(warthog::problem_instance& instance)
		{
            cleanup();
			nodes_expanded_ = nodes_generated_ = 0;
            nodes_touched_ = heap_ops_ = 0;
			search_time_ = 0;

			warthog::timer mytimer;
			mytimer.start();

            // we keep an internal count of how many searches so far
            // (this stuff is used for memory bookkeeping)
			instance.set_search_id(++(this->searchid_));

            // generate the start and goal. then
            // update the instance with their internal ids 
            // (this is just to make debugging easier)
			warthog::search_node* start;
            if(instance.get_start_id() == warthog::INF) { return 0; }
            start = expander_->generate_start_node(&instance);
            instance.set_start_id(start->get_id());

			warthog::search_node* target = 0;
            if(instance.get_target_id() != warthog::INF)
            { 
                target = expander_->generate_target_node(&instance); 
                instance.set_target_id(target->get_id());
                target = 0; // just need the id; FIXME: hacky 
            }


			#ifndef NDEBUG
			if(verbose_)
			{
				std::cerr << "search: startid="<<instance.get_start_id()<<" targetid=" 
                    <<instance.get_target_id() << " (searchid: " << instance.get_search_id()
                    << ")" << std::endl;
			}
			#endif


            int32_t sx, sy, gx, gy;
            expander_->get_xy(instance.get_start_id(), sx, sy);
            expander_->get_xy(instance.get_target_id(), gx, gy);
			start->init(instance.get_search_id(), 0, 0, 
                    heuristic_->h(sx, sy, gx, gy));
			open_->push(start);

			while(open_->size())
			{
				nodes_touched_++;
				if(open_->peek()->get_id() == instance.get_target_id())
				{
					#ifndef NDEBUG
					if(verbose_)
					{
						int32_t x, y;
						warthog::search_node* current = open_->peek();
                        expander_->get_xy(current->get_id(), x, y);
						std::cerr << "target found ("<<x<<", "<<y<<")...";
						current->print(std::cerr);
						std::cerr << std::endl;
					}
					#endif
					target = open_->peek();
					break;
				}

                // early termination tests (in case we want bounded-cost 
                // search or if we want to impose some memory limit)
                if(open_->peek()->get_f() > cost_cutoff_) { break; } 
                if(nodes_expanded_ >= exp_cutoff_) { break; }

				warthog::search_node* current = open_->pop();
                heap_ops_++;
				nodes_expanded_++;

				#ifndef NDEBUG
				if(verbose_)
				{
					int32_t x, y;
                    expander_->get_xy(current->get_id(), x, y);
					std::cerr << this->nodes_expanded_ 
                        << ". expanding ("<<x<<", "<<y<<")...";
					current->print(std::cerr);
					std::cerr << std::endl;
				}
				#endif
				current->set_expanded(true); // NB: set before generating
				assert(current->get_expanded());
				expander_->expand(current, &instance);

				warthog::search_node* n = 0;
				double cost_to_n = warthog::INF;
				for(expander_->first(n, cost_to_n); 
						n != 0;
					   	expander_->next(n, cost_to_n))
				{
                    nodes_touched_++;
					if(n->get_expanded())
					{
						// skip neighbours already expanded
                        #ifndef NDEBUG
                        if(verbose_)
                        {
                            int32_t x, y;
                            expander_->get_xy(n->get_id(), x, y);
                            std::cerr << "  closed; (edgecost=" 
                                << cost_to_n << ") ("<<x<<", "<<y<<")...";
                            n->print(std::cerr);
                            std::cerr << std::endl;
                        }
                        #endif
						continue;
					}

					if(open_->contains(n))
					{
						// update a node from the fringe
						double gval = current->get_g() + cost_to_n;
						if(gval < n->get_g())
						{
							n->relax(gval, current);
							open_->decrease_key(n);
                            heap_ops_++;

							#ifndef NDEBUG
							if(verbose_)
							{
								int32_t x, y;
                                expander_->get_xy(n->get_id(), x, y);
								std::cerr 
                                    << "  open; updating (edgecost="
                                    << cost_to_n<<") ("<<x<<", "<<y<<")...";
								n->print(std::cerr);
								std::cerr << std::endl;
							}
							#endif
                            on_relax_fn_(n);
						}
						else
						{
                            on_norelax_fn_(n, current, cost_to_n);

							#ifndef NDEBUG
							if(verbose_)
							{
								int32_t x, y;
                                expander_->get_xy(n->get_id(), x, y);
								std::cerr 
                                    << "  open; not updating (edgecost=" 
                                    << cost_to_n<< ") ("<<x<<", "<<y<<")...";
								n->print(std::cerr);
								std::cerr << std::endl;
							}
							#endif
						}
					}
					else
					{
						// add a new node to the fringe
						double gval = current->get_g() + cost_to_n;
                        int32_t nx, ny;
                        expander_->get_xy(n->get_id(), nx, ny);
                        n->init(instance.get_search_id(), current, 
                            gval, gval + heuristic_->h(nx, ny, gx, gy));
                        
                        // but only if the node is not provably redundant
                        if(filter_ && filter_->filter(n))
                        {
                            #ifndef NDEBUG
                            if(verbose_)
                            {
                                std::cerr 
                                    << "  filtered-out (edgecost=" 
                                    << cost_to_n<<") ("<<nx<<", "<<ny<<")...";
                                n->print(std::cerr);
                                std::cerr << std::endl;
                            }
                            #endif
                            continue;
                        }

                        open_->push(n);
                        nodes_generated_++;
                        heap_ops_++;

                        #ifndef NDEBUG
                        if(verbose_)
                        {
                            std::cerr 
                                << "  generating (edgecost=" 
                                << cost_to_n<<") ("<< nx <<", "<< ny <<")...";
                            n->print(std::cerr);
                            std::cerr << std::endl;
                        }
                        #endif

                        on_relax_fn_(n);
					}
				}
			}

            #ifndef NDEBUG
            if(verbose_)
            {
                if(target == 0) 
                {
                    std::cerr << "search failed; no solution exists " << std::endl;
                }
            }
            #endif

			mytimer.stop();
			search_time_ = mytimer.elapsed_time_micro();
			return target;
		}

		void
		cleanup()
		{
			open_->clear();
			expander_->clear();
		}
};

}

#endif

