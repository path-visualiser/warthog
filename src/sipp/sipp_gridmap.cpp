#include "sipp/sipp_gridmap.h"
#include <algorithm>

warthog::sipp_gridmap::sipp_gridmap(warthog::gridmap* gm)
    : gm_(gm)
{
    t_gm_ = new warthog::gridmap(gm_->header_height(), gm_->header_width());
    for(uint32_t i = 0; i < t_gm_->width() * t_gm_->height(); i++)
    { t_gm_->set_label(i, 0); }

    uint32_t mapsize = gm_->header_width() * gm_->header_height();
    intervals_.resize(mapsize);
    for(uint32_t i = 0; i < mapsize; i++)
    {
        warthog::sipp::safe_interval si; 
        if(!gm_->get_label(i))
        { si.s_time_ = warthog::COST_MAX; }
        else 
        { si.s_time_ = 0; }

        si.e_time_ = warthog::COST_MAX;
        intervals_.at(i).push_back(warthog::sipp::safe_interval());
    }

//    xy_id_offsets[warthog::cbs::NONE] = 0;
//    xy_id_offsets[warthog::cbs::WAIT] = 0;
//    xy_id_offsets[warthog::cbs::EAST] = 1;
//    xy_id_offsets[warthog::cbs::WEST] = -1;
//    xy_id_offsets[warthog::cbs::SOUTH] = gm_->header_width();
//    xy_id_offsets[warthog::cbs::NORTH] = -1 * gm_->header_width();
//
//    ec_moves[warthog::cbs::NONE] = 0;
//    ec_moves[warthog::cbs::WAIT] = 0;
//    ec_moves[warthog::cbs::NORTH] = warthog::cbs::SOUTH;
//    ec_moves[warthog::cbs::SOUTH] = warthog::cbs::NORTH;
//    ec_moves[warthog::cbs::EAST] = warthog::cbs::WEST;
//    ec_moves[warthog::cbs::WEST] = warthog::cbs::EAST;
}

warthog::sipp_gridmap::~sipp_gridmap()
{
    delete t_gm_;
}


void
warthog::sipp_gridmap::add_obstacle(
    uint32_t x, uint32_t y, cost_t start_time, cost_t end_time, 
    warthog::cbs::move action)
{
    // temporal obstacles need to have a non-zero duration
    if((end_time - start_time) == 0) { return; } 

    uint32_t node_id = y * gm_->header_width() + x;
    std::vector<warthog::sipp::safe_interval> temp;
    for(uint32_t i = 0; i < intervals_.at(node_id).size(); i++)
    {
        warthog::sipp::safe_interval si = intervals_.at(node_id).at(i);

        // these intervals are unaffected by the new obstacle (still safe)
        if(end_time < si.s_time_) { temp.push_back(si); continue; }
        if(start_time > si.e_time_) { temp.push_back(si); continue; }

        // these intervals are dominated by the obstacle so we remove them
        if(start_time <= si.s_time_ && si.e_time_ <= end_time)
        { continue; }

        // these intervals overlap with the obstacle so we adjust them 
        if(start_time <= si.s_time_ && end_time < si.e_time_)
        {
            si.s_time_ = end_time; 
            si.action_  = action;
            temp.push_back(si);
            continue;
        }

        // these intervals overlap with the obstacle so we adjust them 
        if(si.s_time_ < start_time && si.e_time_ <= end_time)
        {
            si.e_time_ = start_time;
            temp.push_back(si);
            continue;
        }

        // these intervals are divided by the obstacle into two 
        if(si.s_time_ < start_time && si.e_time_ > end_time)
        {
            // new safe interval, begins after the end of the obstacle
            warthog::sipp::safe_interval new_si;
            new_si.s_time_ = end_time;
            new_si.e_time_ = si.e_time_;
            new_si.action_ = action;
            temp.push_back(new_si);

            // existing interval, safe only up to the time of the obstacle
            si.e_time_ = start_time;
            temp.push_back(si);
        }
    }

    // sort the new list of safe intervals by start times
    std::sort(temp.begin(), temp.end(), 
        [](warthog::sipp::safe_interval first, 
           warthog::sipp::safe_interval second) -> bool
        {
            return first.s_time_ < second.s_time_;
        });

    // replace the old list with the new list
    intervals_.at(node_id) = temp;

    // record the fact that there are temporal obstacles at this location
    t_gm_->set_label(t_gm_->to_padded_id(node_id), true);
}

void
warthog::sipp_gridmap::clear_obstacles(uint32_t x, uint32_t y)
{
    uint32_t node_id = y*gm_->header_width() + x;
    intervals_.at(node_id).clear();
    warthog::sipp::safe_interval si;

    if(!gm_->get_label(node_id))
    { si.s_time_ = warthog::COST_MAX; }
    else
    { si.s_time_ = 0; }
    si.e_time_ = warthog::COST_MAX;

    intervals_.at(node_id).push_back(si);

    // record the fact that there are no temporal obstacles at this location
    t_gm_->set_label(t_gm_->to_padded_id(node_id), false);
}

