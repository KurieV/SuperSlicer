#include "libslic3r.h"
#include "I18N.hpp"
#include "GCode.hpp"
#include "ExtrusionEntity.hpp"
#include "EdgeGrid.hpp"
#include "Geometry.hpp"
#include "GCode/PrintExtents.hpp"
#include "GCode/WipeTower.hpp"
#include "ShortestPath.hpp"
#include "Utils.hpp"
#include "libslic3r.h"
#include "PrintConfig.hpp"

#include <algorithm>
#include <cstdlib>
#include <math.h>
#include <string_view>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/find.hpp>
#include <boost/foreach.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#if ENABLE_THUMBNAIL_GENERATOR
#include <boost/beast/core/detail/base64.hpp>
#endif // ENABLE_THUMBNAIL_GENERATOR

#include <boost/nowide/iostream.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/cstdlib.hpp>

#include "SVG.hpp"

#include <tbb/parallel_for.h>

#include <Shiny/Shiny.h>

#include "miniz_extension.hpp"

using namespace std::literals::string_view_literals;

#if 0
// Enable debugging and asserts, even in the release build.
#define DEBUG
#define _DEBUG
#undef NDEBUG
#endif

#include <assert.h>

namespace Slic3r {

//! macro used to mark string used at localization,
//! return same string
#define L(s) (s)
#define _(s) Slic3r::I18N::translate(s)

// Only add a newline in case the current G-code does not end with a newline.
static inline void check_add_eol(std::string &gcode)
{
    if (! gcode.empty() && gcode.back() != '\n')
        gcode += '\n';    
}
    

// Return true if tch_prefix is found in custom_gcode
static bool custom_gcode_changes_tool(const std::string& custom_gcode, const std::string& tch_prefix, unsigned next_extruder)
{
    bool ok = false;
    size_t from_pos = 0;
    size_t pos = 0;
    while ((pos = custom_gcode.find(tch_prefix, from_pos)) != std::string::npos) {
        if (pos+1 == custom_gcode.size())
            break;
        from_pos = pos+1;
        // only whitespace is allowed before the command
        while (--pos < custom_gcode.size() && custom_gcode[pos] != '\n') {
            if (! std::isspace(custom_gcode[pos]))
                goto NEXT;
        }
        {
            // we should also check that the extruder changes to what was expected
            std::istringstream ss(custom_gcode.substr(from_pos, std::string::npos));
            unsigned num = 0;
            if (ss >> num)
                ok = (num == next_extruder);
        }
NEXT: ;
    }
    return ok;
}

void AvoidCrossingPerimeters::init_external_mp(const Print &print)
{
	m_external_mp = Slic3r::make_unique<MotionPlanner>(union_ex(this->collect_contours_all_layers(print.objects())));
}

// Plan a travel move while minimizing the number of perimeter crossings.
// point is in unscaled coordinates, in the coordinate system of the current active object
// (set by gcodegen.set_origin()).
Polyline AvoidCrossingPerimeters::travel_to(const GCode &gcodegen, const Point &point) 
{
    // If use_external, then perform the path planning in the world coordinate system (correcting for the gcodegen offset).
    // Otherwise perform the path planning in the coordinate system of the active object.
    bool  use_external  = this->use_external_mp || this->use_external_mp_once;
    Point scaled_origin = use_external ? Point::new_scale(gcodegen.origin()(0), gcodegen.origin()(1)) : Point(0, 0);
    Polyline result = (use_external ? m_external_mp.get() : m_layer_mp.get())->
        shortest_path(gcodegen.last_pos() + scaled_origin, point + scaled_origin);
    if (use_external)
        result.translate(- scaled_origin);
    return result;
}

// Collect outer contours of all objects over all layers.
// Discard objects only containing thin walls (offset would fail on an empty polygon).
// Used by avoid crossing perimeters feature.
Polygons AvoidCrossingPerimeters::collect_contours_all_layers(const PrintObjectPtrs& objects)
{
    Polygons islands;
    for (const PrintObject *object : objects) {
        // Reducing all the object slices into the Z projection in a logarithimc fashion.
        // First reduce to half the number of layers.
        std::vector<Polygons> polygons_per_layer((object->layers().size() + 1) / 2);
         tbb::parallel_for(tbb::blocked_range<size_t>(0, object->layers().size() / 2),
             [&object, &polygons_per_layer](const tbb::blocked_range<size_t> &range) {
                 for (size_t i = range.begin(); i < range.end(); ++ i) {
                     const Layer* layer1 = object->layers()[i * 2];
                     const Layer* layer2 = object->layers()[i * 2 + 1];
                     Polygons polys;
                     polys.reserve(layer1->lslices.size() + layer2->lslices.size());
                     for (const ExPolygon &expoly : layer1->lslices)
                        //FIXME no holes?
                        polys.emplace_back(expoly.contour);
                     for (const ExPolygon &expoly : layer2->lslices)
                        //FIXME no holes?
                        polys.emplace_back(expoly.contour);
                     polygons_per_layer[i] = union_(polys);
                 }
             });
         if (object->layers().size() & 1) {
            const Layer *layer = object->layers().back();
            Polygons polys;
            polys.reserve(layer->lslices.size());
            for (const ExPolygon &expoly : layer->lslices)
                //FIXME no holes?
                polys.emplace_back(expoly.contour);
             polygons_per_layer.back() = union_(polys);
         }
         // Now reduce down to a single layer.
         size_t cnt = polygons_per_layer.size();
         while (cnt > 1) {
             tbb::parallel_for(tbb::blocked_range<size_t>(0, cnt / 2),
                 [&polygons_per_layer](const tbb::blocked_range<size_t> &range) {
                     for (size_t i = range.begin(); i < range.end(); ++ i) {
                         Polygons polys;
                         polys.reserve(polygons_per_layer[i * 2].size() + polygons_per_layer[i * 2 + 1].size());
                         polygons_append(polys, polygons_per_layer[i * 2]);
                         polygons_append(polys, polygons_per_layer[i * 2 + 1]);
                         polygons_per_layer[i * 2] = union_(polys);
                     }
                 });
             for (size_t i = 0; i < cnt / 2; ++ i)
                 polygons_per_layer[i] = std::move(polygons_per_layer[i * 2]);
             if (cnt & 1)
                 polygons_per_layer[cnt / 2] = std::move(polygons_per_layer[cnt - 1]);
             cnt = (cnt + 1) / 2;
         }
         // And collect copies of the objects.
        for (const PrintInstance &instance : object->instances()) {
            // All the layers were reduced to the 1st item of polygons_per_layer.
             size_t i = islands.size();
             polygons_append(islands, polygons_per_layer.front());
             for (; i < islands.size(); ++ i)
                islands[i].translate(instance.shift);
        }
    }
    return islands;
}

std::string OozePrevention::pre_toolchange(GCode &gcodegen)
{
    std::string gcode;
    
    // move to the nearest standby point
    if (!this->standby_points.empty()) {
        // get current position in print coordinates
        Vec3d writer_pos = gcodegen.writer().get_position();
        Point pos = Point::new_scale(writer_pos(0), writer_pos(1));
        
        // find standby point
        Point standby_point;
        pos.nearest_point(this->standby_points, &standby_point);
        
        /*  We don't call gcodegen.travel_to() because we don't need retraction (it was already
            triggered by the caller) nor avoid_crossing_perimeters and also because the coordinates
            of the destination point must not be transformed by origin nor current extruder offset.  */
        gcode += gcodegen.writer().travel_to_xy(unscale(standby_point), 
            "move to standby position");
    }
    
    if (gcodegen.config().standby_temperature_delta.value != 0 && gcodegen.writer().tool_is_extruder()) {
        // we assume that heating is always slower than cooling, so no need to block
        gcode += gcodegen.writer().set_temperature
            (this->_get_temp(gcodegen) + gcodegen.config().standby_temperature_delta.value, false, gcodegen.writer().tool()->id());
    }
    
    return gcode;
}

std::string OozePrevention::post_toolchange(GCode &gcodegen)
{
    return (gcodegen.config().standby_temperature_delta.value != 0 && gcodegen.writer().tool_is_extruder()) ?
        gcodegen.writer().set_temperature(this->_get_temp(gcodegen), true, gcodegen.writer().tool()->id()) :
        std::string();
}

int
OozePrevention::_get_temp(GCode &gcodegen)
{
    if (gcodegen.writer().tool_is_extruder())
        return (gcodegen.layer() != NULL && gcodegen.layer()->id() == 0)
            ? gcodegen.config().first_layer_temperature.get_at(gcodegen.writer().tool()->id())
            : gcodegen.config().temperature.get_at(gcodegen.writer().tool()->id());
    else
        return 0;
}

std::string Wipe::wipe(GCode &gcodegen, bool toolchange)
{
    std::string gcode;
    
    /*  Reduce feedrate a bit; travel speed is often too high to move on existing material.
        Too fast = ripping of existing material; too slow = short wipe path, thus more blob.  */
    double wipe_speed = gcodegen.writer().config.travel_speed.value * 0.8;
    
    // get the retraction length
    double length = toolchange
        ? gcodegen.writer().tool()->retract_length_toolchange()
        : gcodegen.writer().tool()->retract_length();
    // Shorten the retraction length by the amount already retracted before wipe.
    length *= (1. - gcodegen.writer().tool()->retract_before_wipe());

    if (length > 0 && gcodegen.writer().tool()->retract_speed() > 0) {
        /*  Calculate how long we need to travel in order to consume the required
            amount of retraction. In other words, how far do we move in XY at wipe_speed
            for the time needed to consume retract_length at retract_speed?  */
        double wipe_dist = scale_(length / gcodegen.writer().tool()->retract_speed() * wipe_speed);
    
        /*  Take the stored wipe path and replace first point with the current actual position
            (they might be different, for example, in case of loop clipping).  */
        Polyline wipe_path;
        wipe_path.append(gcodegen.last_pos());
        wipe_path.append(
            this->path.points.begin() + 1,
            this->path.points.end()
        );
        
        wipe_path.clip_end(wipe_path.length() - wipe_dist);
    
        // subdivide the retraction in segments
        if (! wipe_path.empty()) {
            for (const Line &line : wipe_path.lines()) {
                double segment_length = line.length();
                /*  Reduce retraction length a bit to avoid effective retraction speed to be greater than the configured one
                    due to rounding (TODO: test and/or better math for this)  */
                double dE = length * (segment_length / wipe_dist) * 0.95;
                //FIXME one shall not generate the unnecessary G1 Fxxx commands, here wipe_speed is a constant inside this cycle.
                // Is it here for the cooling markers? Or should it be outside of the cycle?
                gcode += gcodegen.writer().set_speed(wipe_speed*60, "", gcodegen.enable_cooling_markers() ? ";_WIPE" : "");
                gcode += gcodegen.writer().extrude_to_xy(
                    gcodegen.point_to_gcode(line.b),
                    -dE,
                    "wipe and retract"
                );
            }
			gcodegen.set_last_pos(wipe_path.points.back());
        }
        
        // prevent wiping again on same path
        this->reset_path();
    }
    
    return gcode;
}

//if first layer, ask for a bigger lift for travel to object, to be on the safe side
static inline void set_extra_lift(const Layer& layer, const Print& print, GCodeWriter & writer, int extruder_id) {
    //if first layer, ask for a bigger lift for travel to object, to be on the safe side
    if (layer.id() == 0 && print.config().retract_lift.get_at(extruder_id) != 0) {
        //get biggest first layer height and set extra lift for first travel, to be safe.
        double extra_lift_value = 0;
        for (const PrintObject* obj : print.objects())
            extra_lift_value = std::max(extra_lift_value, obj->config().first_layer_height.get_abs_value(print.config().nozzle_diameter.get_at(0)));
        writer.set_extra_lift(extra_lift_value * 2);
    }
}

static inline Point wipe_tower_point_to_object_point(GCode &gcodegen, const Vec2f &wipe_tower_pt)
{
    return Point(scale_(wipe_tower_pt.x() - gcodegen.origin()(0)), scale_(wipe_tower_pt.y() - gcodegen.origin()(1)));
}

std::string WipeTowerIntegration::append_tcr(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr, int new_extruder_id, double z) const
{
    if (new_extruder_id != -1 && new_extruder_id != tcr.new_tool)
        throw std::invalid_argument("Error: WipeTowerIntegration::append_tcr was asked to do a toolchange it didn't expect.");

    std::string gcode;

    // Toolchangeresult.gcode assumes the wipe tower corner is at the origin (except for priming lines)
    // We want to rotate and shift all extrusions (gcode postprocessing) and starting and ending position
    float alpha = m_wipe_tower_rotation/180.f * float(M_PI);
    Vec2f start_pos = tcr.start_pos;
    Vec2f end_pos = tcr.end_pos;
    if (!tcr.priming) {
        start_pos = Eigen::Rotation2Df(alpha) * start_pos;
        start_pos += m_wipe_tower_pos;
        end_pos = Eigen::Rotation2Df(alpha) * end_pos;
        end_pos += m_wipe_tower_pos;
    }

    Vec2f wipe_tower_offset = tcr.priming ? Vec2f::Zero() : m_wipe_tower_pos;
    float wipe_tower_rotation = tcr.priming ? 0.f : alpha;

    std::string tcr_rotated_gcode = post_process_wipe_tower_moves(tcr, wipe_tower_offset, wipe_tower_rotation);

    if (!tcr.priming) {
        // Move over the wipe tower.
        // Retract for a tool change, using the toolchange retract value and setting the priming extra length.
        gcode += gcodegen.retract(true);
        gcodegen.m_avoid_crossing_perimeters.use_external_mp_once = true;
        gcode += gcodegen.travel_to(
            wipe_tower_point_to_object_point(gcodegen, start_pos),
            erMixed,
            "Travel to a Wipe Tower");
        gcode += gcodegen.unretract();
    }

    double current_z = gcodegen.writer().get_position().z();
    if (z == -1.) // in case no specific z was provided, print at current_z pos
        z = current_z;
    if (! is_approx(z, current_z)) {
        gcode += gcodegen.writer().retract();
        gcode += gcodegen.writer().travel_to_z(z, "Travel down to the last wipe tower layer.");
        gcode += gcodegen.writer().unretract();
    }


    // Process the end filament gcode.
    std::string end_filament_gcode_str;
    if (gcodegen.writer().tool() != nullptr && gcodegen.writer().tool_is_extruder()) {
        // Process the custom end_filament_gcode in case of single_extruder_multi_material.
        unsigned int        old_extruder_id     = gcodegen.writer().tool()->id();
        const std::string  &end_filament_gcode  = gcodegen.config().end_filament_gcode.get_at(old_extruder_id);
        if (gcodegen.writer().tool() != nullptr && ! end_filament_gcode.empty()) {
            DynamicConfig config;
            int previous_extruder_id = gcodegen.writer().tool() ? (int)gcodegen.writer().tool()->id() : -1;
            config.set_key_value("previous_extruder", new ConfigOptionInt(previous_extruder_id));
            config.set_key_value("next_extruder", new ConfigOptionInt((int)new_extruder_id));
            config.set_key_value("layer_num", new ConfigOptionInt(gcodegen.m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(tcr.print_z));
            end_filament_gcode_str = gcodegen.placeholder_parser_process("end_filament_gcode", end_filament_gcode, old_extruder_id, &config);
            check_add_eol(end_filament_gcode_str);
        }
    }

    // Process the custom toolchange_gcode. If it is empty, provide a simple Tn command to change the filament.
    // Otherwise, leave control to the user completely.
    std::string toolchange_gcode_str;
    if (true /*gcodegen.writer().extruder() != nullptr*/) {
        const std::string& toolchange_gcode = gcodegen.config().toolchange_gcode.value;
        if (!toolchange_gcode.empty()) {
            DynamicConfig config;
            int previous_extruder_id = gcodegen.writer().tool() ? (int)gcodegen.writer().tool()->id() : -1;
            config.set_key_value("previous_extruder", new ConfigOptionInt(previous_extruder_id));
            config.set_key_value("next_extruder",     new ConfigOptionInt((int)new_extruder_id));
            config.set_key_value("layer_num",         new ConfigOptionInt(gcodegen.m_layer_index));
            config.set_key_value("layer_z",           new ConfigOptionFloat(tcr.print_z));
            toolchange_gcode_str = gcodegen.placeholder_parser_process("toolchange_gcode", toolchange_gcode, new_extruder_id, &config);
            check_add_eol(toolchange_gcode_str);
        }

        std::string toolchange_command;
        if (tcr.priming || (new_extruder_id >= 0 && gcodegen.writer().need_toolchange(new_extruder_id)))
            toolchange_command = gcodegen.writer().toolchange(new_extruder_id);
        if (! custom_gcode_changes_tool(toolchange_gcode_str, gcodegen.writer().toolchange_prefix(), new_extruder_id))
            toolchange_gcode_str += toolchange_command;
        else {
            // We have informed the m_writer about the current extruder_id, we can ignore the generated G-code.
        }
    }

    gcodegen.placeholder_parser().set("current_extruder", new_extruder_id);

    // Process the start filament gcode.
    std::string start_filament_gcode_str;
    const std::string &start_filament_gcode = gcodegen.config().start_filament_gcode.get_at(new_extruder_id);
    if (! start_filament_gcode.empty()) {
        // Process the start_filament_gcode for the active filament only.
        DynamicConfig config;
        config.set_key_value("filament_extruder_id", new ConfigOptionInt(new_extruder_id));
        config.set_key_value("previous_extruder", new ConfigOptionInt(gcodegen.writer().tool() ? (int)gcodegen.writer().tool()->id() : -1));
        config.set_key_value("next_extruder", new ConfigOptionInt(new_extruder_id));
        config.set_key_value("layer_num", new ConfigOptionInt(0));
        config.set_key_value("layer_z", new ConfigOptionFloat(z));
        start_filament_gcode_str = gcodegen.placeholder_parser_process("start_filament_gcode", start_filament_gcode, new_extruder_id, &config);
        check_add_eol(start_filament_gcode_str);
    }

    // Insert the end filament, toolchange, and start filament gcode into the generated gcode.
    DynamicConfig config;
    config.set_key_value("end_filament_gcode",   new ConfigOptionString(end_filament_gcode_str));
    config.set_key_value("toolchange_gcode",     new ConfigOptionString(toolchange_gcode_str));
    config.set_key_value("start_filament_gcode", new ConfigOptionString(start_filament_gcode_str));
    std::string tcr_gcode, tcr_escaped_gcode = gcodegen.placeholder_parser_process("tcr_rotated_gcode", tcr_rotated_gcode, new_extruder_id, &config);
    unescape_string_cstyle(tcr_escaped_gcode, tcr_gcode);
    gcode += tcr_gcode;
    check_add_eol(toolchange_gcode_str);

    if (gcodegen.writer().tool() && gcodegen.m_config.filament_enable_toolchange_part_fan.values[gcodegen.writer().tool()->id()]) {
        //if the fan may ahve been changed silently by the wipetower, recover it.
        gcode += gcodegen.m_writer.set_fan(gcodegen.m_writer.get_fan(), true);
    }

    // A phony move to the end position at the wipe tower.
    gcodegen.writer().travel_to_xy(end_pos.cast<double>());
    gcodegen.set_last_pos(wipe_tower_point_to_object_point(gcodegen, end_pos));
    if (! is_approx(z, current_z)) {
        gcode += gcodegen.writer().retract();
        gcode += gcodegen.writer().travel_to_z(current_z, "Travel back up to the topmost object layer.");
        gcode += gcodegen.writer().unretract();
    } else {
        // Prepare a future wipe.
        gcodegen.m_wipe.path.points.clear();
        if (new_extruder_id >= 0) {
            // Start the wipe at the current position.
            gcodegen.m_wipe.path.points.emplace_back(wipe_tower_point_to_object_point(gcodegen, end_pos));
            // Wipe end point: Wipe direction away from the closer tower edge to the further tower edge.
            gcodegen.m_wipe.path.points.emplace_back(wipe_tower_point_to_object_point(gcodegen, 
                Vec2f((std::abs(m_left - end_pos.x()) < std::abs(m_right - end_pos.x())) ? m_right : m_left,
                end_pos.y())));
        }
    }

    // Let the planner know we are traveling between objects.
    gcodegen.m_avoid_crossing_perimeters.use_external_mp_once = true;
    return gcode;
}

// This function postprocesses gcode_original, rotates and moves all G1 extrusions and returns resulting gcode
// Starting position has to be supplied explicitely (otherwise it would fail in case first G1 command only contained one coordinate)
std::string WipeTowerIntegration::post_process_wipe_tower_moves(const WipeTower::ToolChangeResult& tcr, const Vec2f& translation, float angle) const
{
    Vec2f extruder_offset = m_extruder_offsets[tcr.initial_tool].cast<float>();

    std::istringstream gcode_str(tcr.gcode);
    std::string gcode_out;
    std::string line;
    Vec2f pos = tcr.start_pos;
    Vec2f transformed_pos = pos;
    Vec2f old_pos(-1000.1f, -1000.1f);
    std::string never_skip_tag = WipeTower::never_skip_tag();

    while (gcode_str) {
        std::getline(gcode_str, line);  // we read the gcode line by line

        // All G1 commands should be translated and rotated. X and Y coords are
        // only pushed to the output when they differ from last time.
        // WT generator can override this by appending the never_skip_tag
        if (line.find("G1 ") == 0) {
            bool never_skip = false;
            auto it = line.find(never_skip_tag);
            if (it != std::string::npos) {
                // remove the tag and remember we saw it
                never_skip = true;
                line.erase(it, it+never_skip_tag.size());
            }
            std::ostringstream line_out;
            std::istringstream line_str(line);
            line_str >> std::noskipws;  // don't skip whitespace
            char ch = 0;
            while (line_str >> ch) {
                if (ch == 'X' || ch =='Y')
                    line_str >> (ch == 'X' ? pos.x() : pos.y());
                else
                        line_out << ch;
            }

            transformed_pos = Eigen::Rotation2Df(angle) * pos + translation;

            if (transformed_pos != old_pos || never_skip) {
                line = line_out.str();
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(3) << "G1 ";
                if (transformed_pos.x() != old_pos.x() || never_skip)
                    oss << " X" << transformed_pos.x() - extruder_offset.x();
                if (transformed_pos.y() != old_pos.y() || never_skip)
                    oss << " Y" << transformed_pos.y() - extruder_offset.y();
                oss << " ";
                line.replace(line.find("G1 "), 3, oss.str());
                old_pos = transformed_pos;
            }
        }

        gcode_out += line + "\n";

        // If this was a toolchange command, we should change current extruder offset
        if (line == "[toolchange_gcode]") {
            extruder_offset = m_extruder_offsets[tcr.new_tool].cast<float>();

            // If the extruder offset changed, add an extra move so everything is continuous
            if (extruder_offset != m_extruder_offsets[tcr.initial_tool].cast<float>()) {
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(3)
                    << "G1 X" << transformed_pos.x() - extruder_offset.x()
                    << " Y"   << transformed_pos.y() - extruder_offset.y()
                    << "\n";
                gcode_out += oss.str();
            }
        }
    }
    return gcode_out;
}


std::string WipeTowerIntegration::prime(GCode &gcodegen)
{
    std::string gcode;


    // Disable linear advance for the wipe tower operations.
        //gcode += (gcodegen.config().gcode_flavor == gcfRepRap || m_gcode_flavor == gcfSprinter ? std::string("M572 D0 S0\n") : std::string("M900 K0\n"));

    for (const WipeTower::ToolChangeResult& tcr : m_priming) {
        if (!tcr.extrusions.empty())
            gcode += append_tcr(gcodegen, tcr, tcr.new_tool);


        // Let the tool change be executed by the wipe tower class.
        // Inform the G-code writer about the changes done behind its back.
        //gcode += tcr.gcode;
        // Let the m_writer know the current extruder_id, but ignore the generated G-code.
  //      unsigned int current_extruder_id = tcr.extrusions.back().tool;
  //      gcodegen.writer().toolchange(current_extruder_id);
  //      gcodegen.placeholder_parser().set("current_extruder", current_extruder_id);

    }

    // A phony move to the end position at the wipe tower.
   /* gcodegen.writer().travel_to_xy(Vec2d(m_priming.back().end_pos.x, m_priming.back().end_pos.y));
    gcodegen.set_last_pos(wipe_tower_point_to_object_point(gcodegen, m_priming.back().end_pos));
    // Prepare a future wipe.
    gcodegen.m_wipe.path.points.clear();
    // Start the wipe at the current position.
    gcodegen.m_wipe.path.points.emplace_back(wipe_tower_point_to_object_point(gcodegen, m_priming.back().end_pos));
    // Wipe end point: Wipe direction away from the closer tower edge to the further tower edge.
    gcodegen.m_wipe.path.points.emplace_back(wipe_tower_point_to_object_point(gcodegen,
        WipeTower::xy((std::abs(m_left - m_priming.back().end_pos.x) < std::abs(m_right - m_priming.back().end_pos.x)) ? m_right : m_left,
        m_priming.back().end_pos.y)));*/

    return gcode;
}

std::string WipeTowerIntegration::tool_change(GCode &gcodegen, int extruder_id, bool finish_layer)
{
    std::string gcode;
    assert(m_layer_idx >= 0);
    if (! m_brim_done || gcodegen.writer().need_toolchange(extruder_id) || finish_layer) {
		if (m_layer_idx < (int)m_tool_changes.size()) {
			if (! (size_t(m_tool_change_idx) < m_tool_changes[m_layer_idx].size()))
                throw std::runtime_error("Wipe tower generation failed, possibly due to empty first layer.");


            // Calculate where the wipe tower layer will be printed. -1 means that print z will not change,
            // resulting in a wipe tower with sparse layers.
            double wipe_tower_z = -1;
            bool ignore_sparse = false;
            if (gcodegen.config().wipe_tower_no_sparse_layers.value) {
                wipe_tower_z = m_last_wipe_tower_print_z;
                ignore_sparse = (m_brim_done && m_tool_changes[m_layer_idx].size() == 1 && m_tool_changes[m_layer_idx].front().initial_tool == m_tool_changes[m_layer_idx].front().new_tool);
                if (m_tool_change_idx == 0 && ! ignore_sparse)
                   wipe_tower_z = m_last_wipe_tower_print_z + m_tool_changes[m_layer_idx].front().layer_height;
		}

            if (! ignore_sparse) {
                gcode += append_tcr(gcodegen, m_tool_changes[m_layer_idx][m_tool_change_idx++], extruder_id, wipe_tower_z);
                m_last_wipe_tower_print_z = wipe_tower_z;
            }
		}
        m_brim_done = true;
    }
    return gcode;
}

// Print is finished. Now it remains to unload the filament safely with ramming over the wipe tower.
std::string WipeTowerIntegration::finalize(GCode &gcodegen)
{
    std::string gcode;
    if (std::abs(gcodegen.writer().get_position()(2) - m_final_purge.print_z) > EPSILON)
        gcode += gcodegen.change_layer(m_final_purge.print_z);
    gcode += append_tcr(gcodegen, m_final_purge, -1);
    return gcode;
}

#define EXTRUDER_CONFIG(OPT) m_config.OPT.get_at(m_writer.extruder()->id())
#define EXTRUDER_CONFIG_WITH_DEFAULT(OPT,DEF) (m_writer.tool_is_extruder()?m_config.OPT.get_at(m_writer.tool()->id()):DEF)
#define BOOL_EXTRUDER_CONFIG(OPT) m_writer.tool_is_extruder() && m_config.OPT.get_at(m_writer.tool()->id())

// Collect pairs of object_layer + support_layer sorted by print_z.
// object_layer & support_layer are considered to be on the same print_z, if they are not further than EPSILON.
std::vector<GCode::LayerToPrint> GCode::collect_layers_to_print(const PrintObject &object)
{
    std::vector<GCode::LayerToPrint> layers_to_print;
    layers_to_print.reserve(object.layers().size() + object.support_layers().size());

    // Calculate a minimum support layer height as a minimum over all extruders, but not smaller than 10um.
    // This is the same logic as in support generator.
    //FIXME should we use the printing extruders instead?
    double gap_over_supports = object.config().support_material_contact_distance_top;
    // FIXME should we test object.config().support_material_synchronize_layers ? IN prusa code, the support layers are synchronized with object layers iff soluble supports.
    assert(! object.config().support_material || gap_over_supports != 0. || object.config().support_material_synchronize_layers);
    if (gap_over_supports != 0.) {
        gap_over_supports = std::max(0., gap_over_supports);
        // Not a soluble support,
        double support_layer_height_min = 1000000.;
        for (auto lh : object.print()->config().min_layer_height.values)
            support_layer_height_min = std::min(support_layer_height_min, std::max(0.01, lh));
        gap_over_supports += support_layer_height_min;
    }

    // Pair the object layers with the support layers by z.
    size_t idx_object_layer  = 0;
    size_t idx_support_layer = 0;
    const LayerToPrint* last_extrusion_layer = nullptr;
    while (idx_object_layer < object.layers().size() || idx_support_layer < object.support_layers().size()) {
        LayerToPrint layer_to_print;
        layer_to_print.object_layer  = (idx_object_layer < object.layers().size()) ? object.layers()[idx_object_layer ++] : nullptr;
        layer_to_print.support_layer = (idx_support_layer < object.support_layers().size()) ? object.support_layers()[idx_support_layer ++] : nullptr;
        if (layer_to_print.object_layer && layer_to_print.support_layer) {
            if (layer_to_print.object_layer->print_z < layer_to_print.support_layer->print_z - EPSILON) {
                layer_to_print.support_layer = nullptr;
                -- idx_support_layer;
            } else if (layer_to_print.support_layer->print_z < layer_to_print.object_layer->print_z - EPSILON) {
                layer_to_print.object_layer = nullptr;
                -- idx_object_layer;
            }
        }

        layers_to_print.emplace_back(layer_to_print);

        // In case there are extrusions on this layer, check there is a layer to lay it on.
        if ((layer_to_print.object_layer && layer_to_print.object_layer->has_extrusions())
            // Allow empty support layers, as the support generator may produce no extrusions for non-empty support regions.
         || (layer_to_print.support_layer /* && layer_to_print.support_layer->has_extrusions() */)) {
            double support_contact_z = (last_extrusion_layer && last_extrusion_layer->support_layer)
                                       ? gap_over_supports
                                       : 0.;
            double maximal_print_z = (last_extrusion_layer ? last_extrusion_layer->print_z() : 0.)
                                    + layer_to_print.layer()->height
                                    + support_contact_z;
            // Negative support_contact_z is not taken into account, it can result in false positives in cases
            // where previous layer has object extrusions too (https://github.com/prusa3d/PrusaSlicer/issues/2752)

            // Only check this layer in case it has some extrusions.
            bool has_extrusions = (layer_to_print.object_layer && layer_to_print.object_layer->has_extrusions())
                               || (layer_to_print.support_layer && layer_to_print.support_layer->has_extrusions());

            if (!object.print()->config().allow_empty_layers && has_extrusions && layer_to_print.print_z() > maximal_print_z + 2. * EPSILON)
                throw std::runtime_error(_(L("Empty layers detected, the output would not be printable.")) + "\n\n" +
                    _(L("Object name")) + ": " + object.model_object()->name + "\n" + _(L("Print z")) + ": " +
                    std::to_string(layers_to_print.back().print_z()) + "\n\n" + _(L("This is "
                    "usually caused by negligibly small extrusions or by a faulty model. Try to repair "
                    "the model or change its orientation on the bed.")));
            // Remember last layer with extrusions.
            last_extrusion_layer = &layers_to_print.back();
    }
    }

    return layers_to_print;
}

// Prepare for non-sequential printing of multiple objects: Support resp. object layers with nearly identical print_z 
// will be printed for  all objects at once.
// Return a list of <print_z, per object LayerToPrint> items.
std::vector<std::pair<coordf_t, std::vector<GCode::LayerToPrint>>> GCode::collect_layers_to_print(const Print &print)
{
    struct OrderingItem {
        coordf_t    print_z;
        size_t      object_idx;
        size_t      layer_idx;
    };

    std::vector<std::vector<LayerToPrint>>  per_object(print.objects().size(), std::vector<LayerToPrint>());
    std::vector<OrderingItem>               ordering;
    for (size_t i = 0; i < print.objects().size(); ++i) {
        per_object[i] = collect_layers_to_print(*print.objects()[i]);
        OrderingItem ordering_item;
        ordering_item.object_idx = i;
        ordering.reserve(ordering.size() + per_object[i].size());
        const LayerToPrint &front = per_object[i].front();
        for (const LayerToPrint &ltp : per_object[i]) {
            ordering_item.print_z   = ltp.print_z();
            ordering_item.layer_idx = &ltp - &front;
            ordering.emplace_back(ordering_item);
        }
    }

    std::sort(ordering.begin(), ordering.end(), [](const OrderingItem &oi1, const OrderingItem &oi2) { return oi1.print_z < oi2.print_z; });

    std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> layers_to_print;
    // Merge numerically very close Z values.
    for (size_t i = 0; i < ordering.size();) {
        // Find the last layer with roughly the same print_z.
        size_t j = i + 1;
        coordf_t zmax = ordering[i].print_z + EPSILON;
        for (; j < ordering.size() && ordering[j].print_z <= zmax; ++ j) ;
        // Merge into layers_to_print.
        std::pair<coordf_t, std::vector<LayerToPrint>> merged;
        // Assign an average print_z to the set of layers with nearly equal print_z.
        merged.first = 0.5 * (ordering[i].print_z + ordering[j-1].print_z);
        merged.second.assign(print.objects().size(), LayerToPrint());
        for (; i < j; ++ i) {
            const OrderingItem &oi = ordering[i];
            assert(merged.second[oi.object_idx].layer() == nullptr);
            merged.second[oi.object_idx] = std::move(per_object[oi.object_idx][oi.layer_idx]);
        }
        layers_to_print.emplace_back(std::move(merged));
    }

    return layers_to_print;
}

#if ENABLE_THUMBNAIL_GENERATOR
void GCode::do_export(Print* print, const char* path, GCodePreviewData* preview_data, ThumbnailsGeneratorCallback thumbnail_cb)
#else
void GCode::do_export(Print *print, const char *path, GCodePreviewData *preview_data)
#endif // ENABLE_THUMBNAIL_GENERATOR
{
    PROFILE_CLEAR();

    // Does the file exist? If so, we hope that it is still valid.
    if (print->is_step_done(psGCodeExport) && boost::filesystem::exists(boost::filesystem::path(path)))
        return;

	print->set_started(psGCodeExport);

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code..." << log_memory_info();

    // Remove the old g-code if it exists.
    boost::nowide::remove(path);

    std::string path_tmp(path);
    path_tmp += ".tmp";

    FILE *file = boost::nowide::fopen(path_tmp.c_str(), "wb");
    if (file == nullptr)
        throw std::runtime_error(std::string("G-code export to ") + path + " failed.\nCannot open the file for writing.\n");

    m_enable_analyzer = preview_data != nullptr;

    try {
        m_placeholder_parser_failed_templates.clear();
#if ENABLE_THUMBNAIL_GENERATOR
        this->_do_export(*print, file, thumbnail_cb);
#else
        this->_do_export(*print, file);
#endif // ENABLE_THUMBNAIL_GENERATOR
        fflush(file);
        if (ferror(file)) {
            fclose(file);
            boost::nowide::remove(path_tmp.c_str());
            throw std::runtime_error(std::string("G-code export to ") + path + " failed\nIs the disk full?\n");
        }
    } catch (std::exception & /* ex */) {
        // Rethrow on any exception. std::runtime_exception and CanceledException are expected to be thrown.
        // Close and remove the file.
        fclose(file);
        boost::nowide::remove(path_tmp.c_str());
        throw;
    }
    fclose(file);

    if (! m_placeholder_parser_failed_templates.empty()) {
        // G-code export proceeded, but some of the PlaceholderParser substitutions failed.
        std::string msg = std::string("G-code export to ") + path + " failed due to invalid custom G-code sections:\n\n";
        for (const std::string &name : m_placeholder_parser_failed_templates)
            msg += std::string("\t") + name + "\n";
        msg += "\nPlease inspect the file ";
        msg += path_tmp + " for error messages enclosed between\n";
        msg += "        !!!!! Failed to process the custom G-code template ...\n";
        msg += "and\n";
        msg += "        !!!!! End of an error report for the custom G-code template ...\n";
        throw std::runtime_error(msg);
    }

    GCodeTimeEstimator::PostProcessData normal_data = m_normal_time_estimator.get_post_process_data();
    GCodeTimeEstimator::PostProcessData silent_data = m_silent_time_estimator.get_post_process_data();

    bool remaining_times_enabled = print->config().remaining_times.value;

    BOOST_LOG_TRIVIAL(debug) << "Time estimator post processing" << log_memory_info();
    GCodeTimeEstimator::post_process(path_tmp, 60.0f, remaining_times_enabled ? &normal_data : nullptr, 
        (remaining_times_enabled && m_silent_time_estimator_enabled) ? &silent_data : nullptr);

    // starts analyzer calculations
    if (m_enable_analyzer) {
        BOOST_LOG_TRIVIAL(debug) << "Preparing G-code preview data" << log_memory_info();
        m_analyzer.calc_gcode_preview_data(*preview_data, m_normal_time_estimator, [print]() { print->throw_if_canceled(); });
        m_analyzer.reset();
    }

    if (remaining_times_enabled)
    {
        m_normal_time_estimator.reset();
        if (m_silent_time_estimator_enabled)
            m_silent_time_estimator.reset();
    }

    if (rename_file(path_tmp, path))
        throw std::runtime_error(
            std::string("Failed to rename the output G-code file from ") + path_tmp + " to " + path + '\n' +
            "Is " + path_tmp + " locked?" + '\n');

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code finished" << log_memory_info();
	print->set_done(psGCodeExport);

    // Write the profiler measurements to file
    PROFILE_UPDATE();
    PROFILE_OUTPUT(debug_out_path("gcode-export-profile.txt").c_str());
}

// free functions called by GCode::_do_export()
namespace DoExport {
	static void init_time_estimators(const PrintConfig &config, GCodeTimeEstimator &normal_time_estimator, GCodeTimeEstimator &silent_time_estimator, bool &silent_time_estimator_enabled)
	{
	    // resets time estimators
	    normal_time_estimator.reset();
	    normal_time_estimator.set_dialect(config.gcode_flavor);
	    normal_time_estimator.set_extrusion_axis(config.get_extrusion_axis()[0]);
	    silent_time_estimator_enabled = (config.gcode_flavor == gcfMarlin) && config.silent_mode;

	    // Until we have a UI support for the other firmwares than the Marlin, use the hardcoded default values
	    // and let the user to enter the G-code limits into the start G-code.
	    // If the following block is enabled for other firmwares than the Marlin, then the function
	    // this->print_machine_envelope(file, print);
	    // shall be adjusted as well to produce a G-code block compatible with the particular firmware flavor.
        // supermerill: done
	    if (true) {
	        normal_time_estimator.set_max_acceleration((float)config.machine_max_acceleration_extruding.values[0]);
	        normal_time_estimator.set_retract_acceleration((float)config.machine_max_acceleration_retracting.values[0]);
            normal_time_estimator.set_max_travel_acceleration((float)config.machine_max_acceleration_travel.values[0]);
	        normal_time_estimator.set_minimum_feedrate((float)config.machine_min_extruding_rate.values[0]);
	        normal_time_estimator.set_minimum_travel_feedrate((float)config.machine_min_travel_rate.values[0]);
	        normal_time_estimator.set_axis_max_acceleration(GCodeTimeEstimator::X, (float)config.machine_max_acceleration_x.values[0]);
	        normal_time_estimator.set_axis_max_acceleration(GCodeTimeEstimator::Y, (float)config.machine_max_acceleration_y.values[0]);
	        normal_time_estimator.set_axis_max_acceleration(GCodeTimeEstimator::Z, (float)config.machine_max_acceleration_z.values[0]);
	        normal_time_estimator.set_axis_max_acceleration(GCodeTimeEstimator::E, (float)config.machine_max_acceleration_e.values[0]);
	        normal_time_estimator.set_axis_max_feedrate(GCodeTimeEstimator::X, (float)config.machine_max_feedrate_x.values[0]);
	        normal_time_estimator.set_axis_max_feedrate(GCodeTimeEstimator::Y, (float)config.machine_max_feedrate_y.values[0]);
	        normal_time_estimator.set_axis_max_feedrate(GCodeTimeEstimator::Z, (float)config.machine_max_feedrate_z.values[0]);
	        normal_time_estimator.set_axis_max_feedrate(GCodeTimeEstimator::E, (float)config.machine_max_feedrate_e.values[0]);
	        normal_time_estimator.set_axis_max_jerk(GCodeTimeEstimator::X, (float)config.machine_max_jerk_x.values[0]);
	        normal_time_estimator.set_axis_max_jerk(GCodeTimeEstimator::Y, (float)config.machine_max_jerk_y.values[0]);
	        normal_time_estimator.set_axis_max_jerk(GCodeTimeEstimator::Z, (float)config.machine_max_jerk_z.values[0]);
	        normal_time_estimator.set_axis_max_jerk(GCodeTimeEstimator::E, (float)config.machine_max_jerk_e.values[0]);

	        if (silent_time_estimator_enabled)
	        {
	            silent_time_estimator.reset();
	            silent_time_estimator.set_dialect(config.gcode_flavor);
	            silent_time_estimator.set_extrusion_axis(config.get_extrusion_axis()[0]);
	            /* "Stealth mode" values can be just a copy of "normal mode" values
	            * (when they aren't input for a printer preset).
	            * Thus, use back value from values, instead of second one, which could be absent
	            */
	            silent_time_estimator.set_max_acceleration((float)config.machine_max_acceleration_extruding.values.back());
	            silent_time_estimator.set_retract_acceleration((float)config.machine_max_acceleration_retracting.values.back());
	            silent_time_estimator.set_minimum_feedrate((float)config.machine_min_extruding_rate.values.back());
	            silent_time_estimator.set_minimum_travel_feedrate((float)config.machine_min_travel_rate.values.back());
	            silent_time_estimator.set_axis_max_acceleration(GCodeTimeEstimator::X, (float)config.machine_max_acceleration_x.values.back());
	            silent_time_estimator.set_axis_max_acceleration(GCodeTimeEstimator::Y, (float)config.machine_max_acceleration_y.values.back());
	            silent_time_estimator.set_axis_max_acceleration(GCodeTimeEstimator::Z, (float)config.machine_max_acceleration_z.values.back());
	            silent_time_estimator.set_axis_max_acceleration(GCodeTimeEstimator::E, (float)config.machine_max_acceleration_e.values.back());
	            silent_time_estimator.set_axis_max_feedrate(GCodeTimeEstimator::X, (float)config.machine_max_feedrate_x.values.back());
	            silent_time_estimator.set_axis_max_feedrate(GCodeTimeEstimator::Y, (float)config.machine_max_feedrate_y.values.back());
	            silent_time_estimator.set_axis_max_feedrate(GCodeTimeEstimator::Z, (float)config.machine_max_feedrate_z.values.back());
	            silent_time_estimator.set_axis_max_feedrate(GCodeTimeEstimator::E, (float)config.machine_max_feedrate_e.values.back());
	            silent_time_estimator.set_axis_max_jerk(GCodeTimeEstimator::X, (float)config.machine_max_jerk_x.values.back());
	            silent_time_estimator.set_axis_max_jerk(GCodeTimeEstimator::Y, (float)config.machine_max_jerk_y.values.back());
	            silent_time_estimator.set_axis_max_jerk(GCodeTimeEstimator::Z, (float)config.machine_max_jerk_z.values.back());
	            silent_time_estimator.set_axis_max_jerk(GCodeTimeEstimator::E, (float)config.machine_max_jerk_e.values.back());
	            if (config.single_extruder_multi_material) {
	                // As of now the fields are shown at the UI dialog in the same combo box as the ramming values, so they
	                // are considered to be active for the single extruder multi-material printers only.
	                silent_time_estimator.set_filament_load_times(config.filament_load_time.values);
	                silent_time_estimator.set_filament_unload_times(config.filament_unload_time.values);
	            }
	        }
	    }
	    // Filament load / unload times are not specific to a firmware flavor. Let anybody use it if they find it useful.
	    if (config.single_extruder_multi_material) {
	        // As of now the fields are shown at the UI dialog in the same combo box as the ramming values, so they
	        // are considered to be active for the single extruder multi-material printers only.
	        normal_time_estimator.set_filament_load_times(config.filament_load_time.values);
	        normal_time_estimator.set_filament_unload_times(config.filament_unload_time.values);
	    }
	}

	static void init_gcode_analyzer(const PrintConfig &config, GCodeAnalyzer &analyzer)
	{
	    // resets analyzer
	    analyzer.reset();

	    // send extruder offset data to analyzer
	    GCodeAnalyzer::ExtruderOffsetsMap extruder_offsets;
	    unsigned int num_extruders = static_cast<unsigned int>(config.nozzle_diameter.values.size());
        for (unsigned int extruder_id = 0; extruder_id < num_extruders; ++extruder_id)
        {
            Vec2d offset = config.extruder_offset.get_at(extruder_id);
            if (!offset.isApprox(Vec2d::Zero()))
                extruder_offsets[extruder_id] = offset;
        }
	    analyzer.set_extruder_offsets(extruder_offsets);

	    // tell analyzer about the extrusion axis
	    analyzer.set_extrusion_axis(config.get_extrusion_axis()[0]);

	    // send extruders count to analyzer to allow it to detect invalid extruder idxs
	    analyzer.set_extruders_count(num_extruders);

	    // tell analyzer about the gcode flavor
	    analyzer.set_gcode_flavor(config.gcode_flavor);
	}

	static double autospeed_volumetric_limit(const Print &print)
	{
	    // get the minimum cross-section used in the print
	    std::vector<double> mm3_per_mm;
	    for (auto object : print.objects()) {
	        for (size_t region_id = 0; region_id < object->region_volumes.size(); ++ region_id) {
	            const PrintRegion* region = print.regions()[region_id];
	            for (auto layer : object->layers()) {
	                const LayerRegion* layerm = layer->regions()[region_id];
	                if (region->config().get_abs_value("perimeter_speed") == 0 ||
	                    region->config().get_abs_value("small_perimeter_speed") == 0 ||
	                    region->config().get_abs_value("external_perimeter_speed") == 0 ||
	                    region->config().get_abs_value("bridge_speed") == 0)
	                    mm3_per_mm.push_back(layerm->perimeters.min_mm3_per_mm());
	                if (region->config().get_abs_value("infill_speed") == 0 ||
	                    region->config().get_abs_value("solid_infill_speed") == 0 ||
	                    region->config().get_abs_value("top_solid_infill_speed") == 0 ||
	                    region->config().get_abs_value("bridge_speed") == 0)
	                    mm3_per_mm.push_back(layerm->fills.min_mm3_per_mm());
	            }
	        }
	        if (object->config().get_abs_value("support_material_speed") == 0 ||
	            object->config().get_abs_value("support_material_interface_speed") == 0)
	            for (auto layer : object->support_layers())
	                mm3_per_mm.push_back(layer->support_fills.min_mm3_per_mm());
	    }
	    // filter out 0-width segments
	    mm3_per_mm.erase(std::remove_if(mm3_per_mm.begin(), mm3_per_mm.end(), [](double v) { return v < 0.000001; }), mm3_per_mm.end());
	    double volumetric_speed = 0.;
	    if (! mm3_per_mm.empty()) {
	        // In order to honor max_print_speed we need to find a target volumetric
	        // speed that we can use throughout the print. So we define this target 
	        // volumetric speed as the volumetric speed produced by printing the 
	        // smallest cross-section at the maximum speed: any larger cross-section
	        // will need slower feedrates.
	        volumetric_speed = *std::min_element(mm3_per_mm.begin(), mm3_per_mm.end()) * print.config().max_print_speed.value;
	        // limit such volumetric speed with max_volumetric_speed if set
	        if (print.config().max_volumetric_speed.value > 0)
	            volumetric_speed = std::min(volumetric_speed, print.config().max_volumetric_speed.value);
	    }
	    return volumetric_speed;
	}

	static void init_ooze_prevention(const Print &print, OozePrevention &ooze_prevention)
	{
	    // Calculate wiping points if needed
	    if (print.config().ooze_prevention.value && ! print.config().single_extruder_multi_material) {
	        Points skirt_points;
	        for (const ExtrusionEntity *ee : print.skirt().entities)
	            for (const ExtrusionPath &path : dynamic_cast<const ExtrusionLoop*>(ee)->paths)
	                append(skirt_points, path.polyline.points);
	        if (! skirt_points.empty()) {
	            Polygon outer_skirt = Slic3r::Geometry::convex_hull(skirt_points);
	            Polygons skirts;
	            for (unsigned int extruder_id : print.extruders()) {
	                const Vec2d &extruder_offset = print.config().extruder_offset.get_at(extruder_id);
	                Polygon s(outer_skirt);
	                s.translate(Point::new_scale(-extruder_offset(0), -extruder_offset(1)));
	                skirts.emplace_back(std::move(s));
	            }
	            ooze_prevention.enable = true;
	            ooze_prevention.standby_points = offset(Slic3r::Geometry::convex_hull(skirts), float(scale_(3.))).front().equally_spaced_points(float(scale_(10.)));
	#if 0
	                require "Slic3r/SVG.pm";
	                Slic3r::SVG::output(
	                    "ooze_prevention.svg",
	                    red_polygons    => \@skirts,
	                    polygons        => [$outer_skirt],
	                    points          => $gcodegen->ooze_prevention->standby_points,
	                );
	#endif
	        }
	    }
	}

	#if ENABLE_THUMBNAIL_GENERATOR
	template<typename WriteToOutput, typename ThrowIfCanceledCallback>
	static void export_thumbnails_to_file(ThumbnailsGeneratorCallback &thumbnail_cb, const std::vector<Vec2d> &sizes, WriteToOutput output, ThrowIfCanceledCallback throw_if_canceled)
	{
	    // Write thumbnails using base64 encoding
	    if (thumbnail_cb != nullptr)
	    {
            std::vector<Vec2d> good_sizes;
            for (const Vec2d &size : sizes)
                if (size.x() > 0 && size.y() > 0)
                    good_sizes.push_back(size);

	        const size_t max_row_length = 78;
	        ThumbnailsList thumbnails;
	        thumbnail_cb(thumbnails, good_sizes, true, true, true, true);
	        for (const ThumbnailData& data : thumbnails)
	        {
	            if (data.is_valid())
	            {
	                size_t png_size = 0;
	                void* png_data = tdefl_write_image_to_png_file_in_memory_ex((const void*)data.pixels.data(), data.width, data.height, 4, &png_size, MZ_DEFAULT_LEVEL, 1);
	                if (png_data != nullptr)
	                {
	                    std::string encoded;
	                    encoded.resize(boost::beast::detail::base64::encoded_size(png_size));
	                    encoded.resize(boost::beast::detail::base64::encode((void*)&encoded[0], (const void*)png_data, png_size));

	                    output((boost::format("\n;\n; thumbnail begin %dx%d %d\n") % data.width % data.height % encoded.size()).str().c_str());

	                    unsigned int row_count = 0;
	                    while (encoded.size() > max_row_length)
	                    {
	                        output((boost::format("; %s\n") % encoded.substr(0, max_row_length)).str().c_str());
	                        encoded = encoded.substr(max_row_length);
	                        ++row_count;
	                    }

	                    if (encoded.size() > 0)
	                    	output((boost::format("; %s\n") % encoded).str().c_str());

	                    output("; thumbnail end\n;\n");

	                    mz_free(png_data);
	                }
	            }
	            throw_if_canceled();
	        }
	    }
	}
	#endif // ENABLE_THUMBNAIL_GENERATOR

	// Fill in print_statistics and return formatted string containing filament statistics to be inserted into G-code comment section.
	static std::string update_print_stats_and_format_filament_stats(
	    const GCodeTimeEstimator    &normal_time_estimator,
	    const GCodeTimeEstimator    &silent_time_estimator,
	    const bool                   silent_time_estimator_enabled,
	    const bool                   has_wipe_tower,
	    const WipeTowerData         &wipe_tower_data,
	    const std::vector<Extruder> &extruders,
		PrintStatistics 		    &print_statistics)
	{
		std::string filament_stats_string_out;

	    print_statistics.clear();
	    print_statistics.estimated_normal_print_time = normal_time_estimator.get_time_dhm/*s*/();
	    print_statistics.estimated_silent_print_time = silent_time_estimator_enabled ? silent_time_estimator.get_time_dhm/*s*/() : "N/A";
	    print_statistics.estimated_normal_custom_gcode_print_times = normal_time_estimator.get_custom_gcode_times();
	    if (silent_time_estimator_enabled)
	        print_statistics.estimated_silent_custom_gcode_print_times = silent_time_estimator.get_custom_gcode_times();
	    print_statistics.total_toolchanges = std::max(0, wipe_tower_data.number_of_toolchanges);
	    if (! extruders.empty()) {
	        std::pair<std::string, unsigned int> out_filament_used_mm ("; filament used [mm] = ", 0);
	        std::pair<std::string, unsigned int> out_filament_used_cm3("; filament used [cm3] = ", 0);
	        std::pair<std::string, unsigned int> out_filament_used_g  ("; filament used [g] = ", 0);
	        std::pair<std::string, unsigned int> out_filament_cost    ("; filament cost = ", 0);
	        for (const Extruder &extruder : extruders) {
	            double used_filament   = extruder.used_filament() + (has_wipe_tower ? wipe_tower_data.used_filament[extruder.id()] : 0.f);
	            double extruded_volume = extruder.extruded_volume() + (has_wipe_tower ? wipe_tower_data.used_filament[extruder.id()] * 2.4052f : 0.f); // assumes 1.75mm filament diameter
	            double filament_weight = extruded_volume * extruder.filament_density() * 0.001;
	            double filament_cost   = filament_weight * extruder.filament_cost()    * 0.001;
	            auto append = [&extruder, &extruders](std::pair<std::string, unsigned int> &dst, const char *tmpl, double value) {
	                while (dst.second < extruder.id()) {
	                    // Fill in the non-printing extruders with zeros.
	                    dst.first += (dst.second > 0) ? ", 0" : "0";
	                    ++ dst.second;
	                }
	                if (dst.second > 0)
	                    dst.first += ", ";
	                char buf[64];
					sprintf(buf, tmpl, value);
	                dst.first += buf;
	                ++ dst.second;
	            };
	            print_statistics.filament_stats.insert(std::pair<size_t, float>{extruder.id(), (float)used_filament});
	            append(out_filament_used_mm,  "%.1lf", used_filament);
	            append(out_filament_used_cm3, "%.1lf", extruded_volume * 0.001);
	            if (filament_weight > 0.) {
	                print_statistics.total_weight = print_statistics.total_weight + filament_weight;
	                append(out_filament_used_g, "%.1lf", filament_weight);
	                if (filament_cost > 0.) {
	                    print_statistics.total_cost = print_statistics.total_cost + filament_cost;
	                    append(out_filament_cost, "%.1lf", filament_cost);
	                }
	            }
	            print_statistics.total_used_filament += used_filament;
	            print_statistics.total_extruded_volume += extruded_volume;
	            print_statistics.total_wipe_tower_filament += has_wipe_tower ? used_filament - extruder.used_filament() : 0.;
	            print_statistics.total_wipe_tower_cost += has_wipe_tower ? (extruded_volume - extruder.extruded_volume())* extruder.filament_density() * 0.001 * extruder.filament_cost() * 0.001 : 0.;
	        }
	        filament_stats_string_out += out_filament_used_mm.first;
            filament_stats_string_out += "\n" + out_filament_used_cm3.first;
			if (out_filament_used_g.second)
                filament_stats_string_out += "\n" + out_filament_used_g.first;
			if (out_filament_cost.second)
                filament_stats_string_out += "\n" + out_filament_cost.first;
	    }
	    return filament_stats_string_out;
	}
}

// Sort the PrintObjects by their increasing Z, likely useful for avoiding colisions on Deltas during sequential prints.
static inline std::vector<const PrintInstance*> sort_object_instances_by_max_z(const Print& print)
{
    std::vector<const PrintObject*> objects(print.objects().begin(), print.objects().end());
    std::sort(objects.begin(), objects.end(), [](const PrintObject* po1, const PrintObject* po2) { return po1->height() < po2->height(); });
    std::vector<const PrintInstance*> instances;
    instances.reserve(objects.size());
    for (const PrintObject* object : objects)
        for (size_t i = 0; i < object->instances().size(); ++i)
            instances.emplace_back(&object->instances()[i]);
    return instances;
}

// Sort the PrintObjects by their increasing Y, likely useful for avoiding colisions on printer with a x-bar during sequential prints.
static inline std::vector<const PrintInstance*> sort_object_instances_by_max_y(const Print& print)
{
    std::vector<const PrintObject*> objects(print.objects().begin(), print.objects().end());
    std::sort(objects.begin(), objects.end(), [](const PrintObject* po1, const PrintObject* po2) { return po1->height() < po2->height(); });
    std::vector<const PrintInstance*> instances;
    instances.reserve(objects.size());
    std::map<const PrintInstance*, coord_t> map_min_y;
    for (const PrintObject* object : objects) {
        for (size_t i = 0; i < object->instances().size(); ++i) {
            instances.emplace_back(&object->instances()[i]);
            // Calculate the convex hull of a printable object. 
            Polygon poly = object->model_object()->convex_hull_2d(
                Geometry::assemble_transform(Vec3d::Zero(),
                    object->instances()[i].model_instance->get_rotation(), 
                    object->instances()[i].model_instance->get_scaling_factor(), 
                    object->instances()[i].model_instance->get_mirror()));
            poly.translate(object->instances()[i].shift - object->center_offset());
            coord_t min_y = poly.first_point().y();
            for (const Point& point : poly.points)
                if (point.y() < min_y)
                    min_y = point.y();
            map_min_y[instances.back()] = min_y;
        }
    }
    std::sort(instances.begin(), instances.end(), [&map_min_y](const PrintInstance* po1, const PrintInstance* po2) { return map_min_y[po1] < map_min_y[po2]; });
    return instances;
}

// Produce a vector of PrintObjects in the order of their respective ModelObjects in print.model().
std::vector<const PrintInstance*> sort_object_instances_by_model_order(const Print& print)
{
    // Build up map from ModelInstance* to PrintInstance*
    std::vector<std::pair<const ModelInstance*, const PrintInstance*>> model_instance_to_print_instance;
    model_instance_to_print_instance.reserve(print.num_object_instances());
    for (const PrintObject *print_object : print.objects())
        for (const PrintInstance &print_instance : print_object->instances())
            model_instance_to_print_instance.emplace_back(print_instance.model_instance, &print_instance);
    std::sort(model_instance_to_print_instance.begin(), model_instance_to_print_instance.end(), [](auto &l, auto &r) { return l.first < r.first; });

    std::vector<const PrintInstance*> instances;
    instances.reserve(model_instance_to_print_instance.size());
    for (const ModelObject *model_object : print.model().objects)
        for (const ModelInstance *model_instance : model_object->instances) {
            auto it = std::lower_bound(model_instance_to_print_instance.begin(), model_instance_to_print_instance.end(), std::make_pair(model_instance, nullptr), [](auto &l, auto &r) { return l.first < r.first; });
            if (it != model_instance_to_print_instance.end() && it->first == model_instance)
                instances.emplace_back(it->second);
        }
    return instances;
}

// set standby temp for extruders
// Parse the custom G-code, try to find T, and add it if not present
static void init_multiextruders(FILE *file, Print &print, GCodeWriter & writer,  ToolOrdering &tool_ordering, const std::string &custom_gcode )
{

    //set standby temp for reprap
    if (std::set<uint8_t>{gcfRepRap}.count(print.config().gcode_flavor.value) > 0) {
        for (uint16_t tool_id : tool_ordering.all_extruders()) {
            fprintf(file, "G10 P%d R%d S%d ; sets the standby temperature\n",
                tool_id,
                int(print.config().filament_toolchange_temp.get_at(tool_id)),
                int(print.config().temperature.get_at(tool_id)));
        }
    }
}


#if ENABLE_THUMBNAIL_GENERATOR
void GCode::_do_export(Print& print, FILE* file, ThumbnailsGeneratorCallback thumbnail_cb)
#else
void GCode::_do_export(Print &print, FILE *file)
#endif // ENABLE_THUMBNAIL_GENERATOR
{
    PROFILE_FUNC();

    DoExport::init_time_estimators(print.config(), 
    	// modifies the following:
    	m_normal_time_estimator, m_silent_time_estimator, m_silent_time_estimator_enabled);
    DoExport::init_gcode_analyzer(print.config(), m_analyzer);

    // resets analyzer's tracking data
    m_last_mm3_per_mm = GCodeAnalyzer::Default_mm3_per_mm;
    m_last_width = GCodeAnalyzer::Default_Width;
    m_last_height = GCodeAnalyzer::Default_Height;
    print.m_print_statistics.color_extruderid_to_used_filament.clear();
    print.m_print_statistics.color_extruderid_to_used_weight.clear();

    // How many times will be change_layer() called?
    // change_layer() in turn increments the progress bar status.
    m_layer_count = 0;
    if (print.config().complete_objects.value) {
        // Add each of the object's layers separately.
        for (auto object : print.objects()) {
            std::vector<coordf_t> zs;
            zs.reserve(object->layers().size() + object->support_layers().size());
            for (auto layer : object->layers())
                zs.push_back(layer->print_z);
            for (auto layer : object->support_layers())
                zs.push_back(layer->print_z);
            std::sort(zs.begin(), zs.end());
            m_layer_count += (unsigned int)(object->instances().size() * (std::unique(zs.begin(), zs.end()) - zs.begin()));
        }
    } else {
        // Print all objects with the same print_z together.
        std::vector<coordf_t> zs;
        for (auto object : print.objects()) {
            zs.reserve(zs.size() + object->layers().size() + object->support_layers().size());
            for (auto layer : object->layers())
                zs.push_back(layer->print_z);
            for (auto layer : object->support_layers())
                zs.push_back(layer->print_z);
        }
        std::sort(zs.begin(), zs.end());
        m_layer_count = (unsigned int)(std::unique(zs.begin(), zs.end()) - zs.begin());
    }
    print.throw_if_canceled();

    m_enable_cooling_markers = true;
    this->apply_print_config(print.config());

    m_volumetric_speed = DoExport::autospeed_volumetric_limit(print);
    print.throw_if_canceled();

    m_cooling_buffer = make_unique<CoolingBuffer>(*this);
    if (print.config().spiral_vase.value)
        m_spiral_vase = make_unique<SpiralVase>(print.config());
#ifdef HAS_PRESSURE_EQUALIZER
    if (print.config().max_volumetric_extrusion_rate_slope_positive.value > 0 ||
        print.config().max_volumetric_extrusion_rate_slope_negative.value > 0)
        m_pressure_equalizer = make_unique<PressureEqualizer>(&print.config());
    m_enable_extrusion_role_markers = (bool)m_pressure_equalizer;
#else /* HAS_PRESSURE_EQUALIZER */
    m_enable_extrusion_role_markers = false;
#endif /* HAS_PRESSURE_EQUALIZER */

    // Write information on the generator.
    _write_format(file, "; %s\n\n", Slic3r::header_slic3r_generated().c_str());

#if ENABLE_THUMBNAIL_GENERATOR
    DoExport::export_thumbnails_to_file(thumbnail_cb, print.full_print_config().option<ConfigOptionPoints>("thumbnails")->values, 
        [this, file](const char* sz) { this->_write(file, sz); }, 
        [&print]() { print.throw_if_canceled(); });
#endif // ENABLE_THUMBNAIL_GENERATOR

    // Write notes (content of the Print Settings tab -> Notes)
    {
        std::list<std::string> lines;
        boost::split(lines, print.config().notes.value, boost::is_any_of("\n"), boost::token_compress_off);
        for (auto line : lines) {
            // Remove the trailing '\r' from the '\r\n' sequence.
            if (! line.empty() && line.back() == '\r')
                line.pop_back();
            _write_format(file, "; %s\n", line.c_str());
        }
        if (! lines.empty())
            _write(file, "\n");
    }
    print.throw_if_canceled();

    // Write some terse information on the slicing parameters.
    const PrintObject *first_object         = print.objects().front();
    const double       layer_height         = first_object->config().layer_height.value;
    const double       first_layer_height   = first_object->config().first_layer_height.get_abs_value(m_config.nozzle_diameter.empty()?0.:m_config.nozzle_diameter.get_at(0));
    for (const PrintRegion* region : print.regions()) {
        _write_format(file, "; external perimeters extrusion width = %.2fmm\n", region->flow(frExternalPerimeter, layer_height, false, false, -1., *first_object).width);
        _write_format(file, "; perimeters extrusion width = %.2fmm\n",          region->flow(frPerimeter,         layer_height, false, false, -1., *first_object).width);
        _write_format(file, "; infill extrusion width = %.2fmm\n",              region->flow(frInfill,            layer_height, false, false, -1., *first_object).width);
        _write_format(file, "; solid infill extrusion width = %.2fmm\n",        region->flow(frSolidInfill,       layer_height, false, false, -1., *first_object).width);
        _write_format(file, "; top infill extrusion width = %.2fmm\n",          region->flow(frTopSolidInfill,    layer_height, false, false, -1., *first_object).width);
        if (print.has_support_material())
            _write_format(file, "; support material extrusion width = %.2fmm\n", support_material_flow(first_object).width);
        if (print.config().first_layer_extrusion_width.value > 0)
            _write_format(file, "; first layer extrusion width = %.2fmm\n",   region->flow(frPerimeter, first_layer_height, false, true, -1., *first_object).width);
        _write_format(file, "\n");
    }
    if (this->config().gcode_label_objects) {
        for (PrintObject *print_object : print.objects()) {
            this->m_ordered_objects.push_back(print_object);
            unsigned int copy_id = 0;
            for (const PrintInstance &print_instance : print_object->instances()) {
                std::string object_name = print_object->model_object()->name;
                size_t pos_dot = object_name.find(".", 0);
                if (pos_dot != std::string::npos && pos_dot > 0)
                    object_name = object_name.substr(0, pos_dot);
                //get bounding box for the instance
                BoundingBoxf3 raw_bbox = print_object->model_object()->raw_mesh_bounding_box();
                BoundingBoxf3 m_bounding_box = print_instance.model_instance->transform_bounding_box(raw_bbox);
                _write_format(file, "; object:{\"name\":\"%s\",\"id\":\"%s id:%d copy %d\",\"object_center\":[%f,%f,%f],\"boundingbox_center\":[%f,%f,%f],\"boundingbox_size\":[%f,%f,%f]}\n",
                    object_name.c_str(), print_object->model_object()->name.c_str(), this->m_ordered_objects.size() - 1, copy_id,
                    m_bounding_box.center().x(), m_bounding_box.center().y(), 0,
                    m_bounding_box.center().x(), m_bounding_box.center().y(), m_bounding_box.center().z(),
                    m_bounding_box.size().x(), m_bounding_box.size().y(), m_bounding_box.size().z()
                );
                copy_id++;
            }
        }
        _write_format(file, "\n");
    }

    print.throw_if_canceled();
    
    // adds tags for time estimators
    if (print.config().remaining_times.value)
    {
        _writeln(file, GCodeTimeEstimator::Normal_First_M73_Output_Placeholder_Tag);
        if (m_silent_time_estimator_enabled)
            _writeln(file, GCodeTimeEstimator::Silent_First_M73_Output_Placeholder_Tag);
    }

    // Prepare the helper object for replacing placeholders in custom G-code and output filename.
    m_placeholder_parser = print.placeholder_parser();
    m_placeholder_parser.update_timestamp();
    print.update_object_placeholders(m_placeholder_parser.config_writable(), ".gcode");

    // Get optimal tool ordering to minimize tool switches of a multi-exruder print.
    // For a print by objects, find the 1st printing object.
    ToolOrdering tool_ordering;
    unsigned int initial_extruder_id = (unsigned int)-1;
    unsigned int final_extruder_id   = (unsigned int)-1;
    bool         has_wipe_tower      = false;
    std::vector<const PrintInstance*> 					print_object_instances_ordering;
    std::vector<const PrintInstance*>::const_iterator 	print_object_instance_sequential_active;
    bool has_milling = false;
    if (!config().milling_diameter.values.empty()) {
        for (const PrintObject* obj : print.objects()) {
            for (const Layer *layer : obj->layers()) {
                for (const LayerRegion *lr : layer->regions()) {
                    if (!lr->milling.empty()) {
                        has_milling = true;
                        break;
                    }
                }
            }
        }
    }
    if (print.config().complete_objects.value) {
        // Order object instances for sequential print.
        if(print.config().complete_objects_sort.value == cosObject)
            print_object_instances_ordering = sort_object_instances_by_model_order(print);
        else if (print.config().complete_objects_sort.value == cosZ)
            print_object_instances_ordering = sort_object_instances_by_max_z(print);
        else if (print.config().complete_objects_sort.value == cosY)
            print_object_instances_ordering = sort_object_instances_by_max_y(print);
        // Find the 1st printing object, find its tool ordering and the initial extruder ID.
        print_object_instance_sequential_active = print_object_instances_ordering.begin();
        for (; print_object_instance_sequential_active != print_object_instances_ordering.end(); ++ print_object_instance_sequential_active) {
            tool_ordering = ToolOrdering(*(*print_object_instance_sequential_active)->print_object, initial_extruder_id);
            if ((initial_extruder_id = tool_ordering.first_extruder()) != static_cast<unsigned int>(-1))
                break;
        }
        // We don't allow switching of extruders per layer by Model::custom_gcode_per_print_z in sequential mode.
        // Use the extruder IDs collected from Regions.
    	this->set_extruders(print.extruders());
        if(has_milling)
            m_writer.set_mills(std::vector<uint16_t>() = { 0 });
    } else {
        // Find tool ordering for all the objects at once, and the initial extruder ID.
        // If the tool ordering has been pre-calculated by Print class for wipe tower already, reuse it.
		tool_ordering = print.tool_ordering();
		tool_ordering.assign_custom_gcodes(print);
        has_wipe_tower = print.has_wipe_tower() && tool_ordering.has_wipe_tower();
        initial_extruder_id = (has_wipe_tower && ! print.config().single_extruder_multi_material_priming) ?
            // The priming towers will be skipped.
            tool_ordering.all_extruders().back() :
            // Don't skip the priming towers.
            tool_ordering.first_extruder();
        // In non-sequential print, the printing extruders may have been modified by the extruder switches stored in Model::custom_gcode_per_print_z.
        // Therefore initialize the printing extruders from there.
    	this->set_extruders(tool_ordering.all_extruders());
        if (has_milling)
            m_writer.set_mills(std::vector<uint16_t>() = { 0 });
        // Order object instances using a nearest neighbor search.
        print_object_instances_ordering = chain_print_object_instances(print);
    }
    if (initial_extruder_id == (unsigned int)-1) {
        // Nothing to print!
        initial_extruder_id = 0;
        final_extruder_id   = 0;
    } else {
        final_extruder_id = tool_ordering.last_extruder();
        assert(final_extruder_id != (unsigned int)-1);
    }
    print.throw_if_canceled();

    m_cooling_buffer->set_current_extruder(initial_extruder_id);

    // Emit machine envelope limits for the Marlin firmware.
    this->print_machine_envelope(file, print);

    // Disable fan.
    if ( print.config().disable_fan_first_layers.get_at(initial_extruder_id)
        && config().gcode_flavor != gcfKlipper)
        _write(file, m_writer.set_fan(0, true));

    // Let the start-up script prime the 1st printing tool.
    m_placeholder_parser.set("initial_tool", initial_extruder_id);
    m_placeholder_parser.set("initial_extruder", initial_extruder_id);
    m_placeholder_parser.set("current_extruder", initial_extruder_id);
    //Set variable for total layer count so it can be used in custom gcode.
    m_placeholder_parser.set("total_layer_count", m_layer_count);
    // Useful for sequential prints.
    m_placeholder_parser.set("current_object_idx", 0);
    // For the start / end G-code to do the priming and final filament pull in case there is no wipe tower provided.
    m_placeholder_parser.set("has_wipe_tower", has_wipe_tower);
    m_placeholder_parser.set("has_single_extruder_multi_material_priming", has_wipe_tower && print.config().single_extruder_multi_material_priming);
    m_placeholder_parser.set("total_toolchanges", std::max(0, print.wipe_tower_data().number_of_toolchanges)); // Check for negative toolchanges (single extruder mode) and set to 0 (no tool change).

    std::string start_gcode = this->placeholder_parser_process("start_gcode", print.config().start_gcode.value, initial_extruder_id);
    // Set bed temperature if the start G-code does not contain any bed temp control G-codes.
    this->_print_first_layer_bed_temperature(file, print, start_gcode, initial_extruder_id, true);

    //init extruders
    init_multiextruders(file, print, m_writer, tool_ordering, start_gcode);

    // Set extruder(s) temperature before and after start G-code.
    this->_print_first_layer_extruder_temperatures(file, print, start_gcode, initial_extruder_id, false);

    if (m_enable_analyzer)
        // adds tag for analyzer
        _write_format(file, ";%s%d\n", GCodeAnalyzer::Extrusion_Role_Tag.c_str(), erCustom);

    // Write the custom start G-code
    _writeln(file, start_gcode);

    // Process filament-specific gcode.
   /* if (has_wipe_tower) {
        // Wipe tower will control the extruder switching, it will call the start_filament_gcode.
    } else {
            DynamicConfig config;
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(initial_extruder_id)));
            _writeln(file, this->placeholder_parser_process("start_filament_gcode", print.config().start_filament_gcode.values[initial_extruder_id], initial_extruder_id, &config));
    }
*/
    this->_print_first_layer_extruder_temperatures(file, print, start_gcode, initial_extruder_id, true);
    print.throw_if_canceled();

    // Set other general things.
    _write(file, this->preamble());

    // Initialize a motion planner for object-to-object travel moves.
    m_avoid_crossing_perimeters.reset();
    if (print.config().avoid_crossing_perimeters.value) {
        m_avoid_crossing_perimeters.init_external_mp(print);
        print.throw_if_canceled();
    }

    // Calculate wiping points if needed
    DoExport::init_ooze_prevention(print, m_ooze_prevention);
    print.throw_if_canceled();

    //activate first extruder is multi-extruder and not in start-gcode
    if (m_writer.multiple_extruders) {
        //if not in gcode
        bool find = false;
        if (!start_gcode.empty()) {
            const char *ptr = start_gcode.data();
            while (*ptr != 0) {
                // Skip whitespaces.
                for (; *ptr == ' ' || *ptr == '\t'; ++ptr);
                if (*ptr == 'T') {
                    // TX for most of the firmwares
                    find = true;
                    break;
                } else if (*ptr == 'A' && print.config().gcode_flavor.value == gcfKlipper) {
                    // ACTIVATE_EXTRUDER for klipper (if used)
                    if (std::string::npos != start_gcode.find("ACTIVATE_EXTRUDER", size_t(ptr - start_gcode.data()))) {
                        find = true;
                        break;
                    }
                }
                // Skip the rest of the line.
                for (; *ptr != 0 && *ptr != '\r' && *ptr != '\n'; ++ptr);
                // Skip the end of line indicators.
                for (; *ptr == '\r' || *ptr == '\n'; ++ptr);
            }
        }
        if (!find) {
            // Set initial extruder only after custom start G-code.
            // Ugly hack: Do not set the initial extruder if the extruder is primed using the MMU priming towers at the edge of the print bed.
            if (!(has_wipe_tower && print.config().single_extruder_multi_material_priming)) {
                _write(file, this->set_extruder(initial_extruder_id, 0.));
            } else {
                m_writer.toolchange(initial_extruder_id);
            }
        } else {
            // set writer to the tool as should be set in the start_gcode.
            _write(file, this->set_extruder(initial_extruder_id, 0., true));
        }
    } else {
        // if we are running a single-extruder setup, just set the extruder and "return nothing"
        _write(file, this->set_extruder(initial_extruder_id, 0.));
    }

    // Do all objects for each layer.
    if (print.config().complete_objects.value) {
        size_t finished_objects = 0;
        const PrintObject *prev_object = (*print_object_instance_sequential_active)->print_object;
        for (; print_object_instance_sequential_active != print_object_instances_ordering.end(); ++ print_object_instance_sequential_active) {
            const PrintObject &object = *(*print_object_instance_sequential_active)->print_object;
            if (&object != prev_object || tool_ordering.first_extruder() != final_extruder_id) {
                tool_ordering = ToolOrdering(object, final_extruder_id);
                unsigned int new_extruder_id = tool_ordering.first_extruder();
                if (new_extruder_id == (unsigned int)-1)
                    // Skip this object.
                    continue;
                initial_extruder_id = new_extruder_id;
                final_extruder_id   = tool_ordering.last_extruder();
                assert(final_extruder_id != (unsigned int)-1);
            }
            print.throw_if_canceled();
            this->set_origin(unscale((*print_object_instance_sequential_active)->shift));
            if (finished_objects > 0) {
                // Move to the origin position for the copy we're going to print.
                // This happens before Z goes down to layer 0 again, so that no collision happens hopefully.
                m_enable_cooling_markers = false; // we're not filtering these moves through CoolingBuffer
                m_avoid_crossing_perimeters.use_external_mp_once = true;
                _write(file, this->retract());
                _write(file, this->travel_to(Point(0, 0), erNone, "move to origin position for next object"));
                m_enable_cooling_markers = true;
                // Disable motion planner when traveling to first object point.
                m_avoid_crossing_perimeters.disable_once = true;
                // Ff we are printing the bottom layer of an object, and we have already finished
                // another one, set first layer temperatures. This happens before the Z move
                // is triggered, so machine has more time to reach such temperatures.
                m_placeholder_parser.set("current_object_idx", int(finished_objects));
                std::string between_objects_gcode = this->placeholder_parser_process("between_objects_gcode", print.config().between_objects_gcode.value, initial_extruder_id);
                // Set first layer bed and extruder temperatures, don't wait for it to reach the temperature.
                this->_print_first_layer_bed_temperature(file, print, between_objects_gcode, initial_extruder_id, false);
                this->_print_first_layer_extruder_temperatures(file, print, between_objects_gcode, initial_extruder_id, false);
                _writeln(file, between_objects_gcode);
            }
            // Reset the cooling buffer internal state (the current position, feed rate, accelerations).
            m_cooling_buffer->reset();
            m_cooling_buffer->set_current_extruder(initial_extruder_id);
            // Pair the object layers with the support layers by z, extrude them.
            std::vector<LayerToPrint> layers_to_print = collect_layers_to_print(object);
            for (const LayerToPrint &ltp : layers_to_print) {
                std::vector<LayerToPrint> lrs;
                lrs.emplace_back(std::move(ltp));
                this->process_layer(file, print, print.m_print_statistics, lrs, tool_ordering.tools_for_layer(ltp.print_z()), nullptr, *print_object_instance_sequential_active - object.instances().data());
                print.throw_if_canceled();
            }
#ifdef HAS_PRESSURE_EQUALIZER
            if (m_pressure_equalizer)
                _write(file, m_pressure_equalizer->process("", true));
#endif /* HAS_PRESSURE_EQUALIZER */
            ++ finished_objects;
            // Flag indicating whether the nozzle temperature changes from 1st to 2nd layer were performed.
            // Reset it when starting another object from 1st layer.
            m_second_layer_things_done = false;
            prev_object = &object;
        }
    } else {
        // Sort layers by Z.
        // All extrusion moves with the same top layer height are extruded uninterrupted.
        std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> layers_to_print = collect_layers_to_print(print);
        // Prusa Multi-Material wipe tower.
        if (has_wipe_tower && ! layers_to_print.empty()) {
            m_wipe_tower.reset(new WipeTowerIntegration(print.config(), *print.wipe_tower_data().priming.get(), print.wipe_tower_data().tool_changes, *print.wipe_tower_data().final_purge.get()));
            _write(file, m_writer.travel_to_z(first_layer_height + m_config.z_offset.value, "Move to the first layer height"));
            if (print.config().single_extruder_multi_material_priming) {
                _write(file, m_wipe_tower->prime(*this));
                // Verify, whether the print overaps the priming extrusions.
                BoundingBoxf bbox_print(get_print_extrusions_extents(print));
                coordf_t twolayers_printz = ((layers_to_print.size() == 1) ? layers_to_print.front() : layers_to_print[1]).first + EPSILON;
                for (const PrintObject *print_object : print.objects())
                    bbox_print.merge(get_print_object_extrusions_extents(*print_object, twolayers_printz));
                bbox_print.merge(get_wipe_tower_extrusions_extents(print, twolayers_printz));
                BoundingBoxf bbox_prime(get_wipe_tower_priming_extrusions_extents(print));
                bbox_prime.offset(0.5f);
                // Beep for 500ms, tone 800Hz. Yet better, play some Morse.
                _write(file, this->retract());
                if(print.config().gcode_flavor.value != gcfKlipper)
                    _write(file, "M300 S800 P500\n");
                if (bbox_prime.overlap(bbox_print)) {
                    // Wait for the user to remove the priming extrusions, otherwise they would
                    // get covered by the print.
                    _write(file, "M1 Remove priming towers and click button.\n");
                }
                else {
                    // Just wait for a bit to let the user check, that the priming succeeded.
                    //TODO Add a message explaining what the printer is waiting for. This needs a firmware fix.
                    _write(file, "M1 S10\n");
                }
            }
            print.throw_if_canceled();
        }
        // Extrude the layers.
        for (auto &layer : layers_to_print) {
            const LayerTools &layer_tools = tool_ordering.tools_for_layer(layer.first);
            if (m_wipe_tower && layer_tools.has_wipe_tower)
                m_wipe_tower->next_layer();
            this->process_layer(file, print, print.m_print_statistics, layer.second, layer_tools, &print_object_instances_ordering, size_t(-1));
            print.throw_if_canceled();
        }
#ifdef HAS_PRESSURE_EQUALIZER
        if (m_pressure_equalizer)
            _write(file, m_pressure_equalizer->process("", true));
#endif /* HAS_PRESSURE_EQUALIZER */
        if (m_wipe_tower)
            // Purge the extruder, pull out the active filament.
            _write(file, m_wipe_tower->finalize(*this));
    }

    // Write end commands to file.
    _write(file, this->retract());
    _write(file, m_writer.set_fan(false));

    if (m_enable_analyzer)
        // adds tag for analyzer
        _write_format(file, ";%s%d\n", GCodeAnalyzer::Extrusion_Role_Tag.c_str(), erCustom);

    // Process filament-specific gcode in extruder order.
    {
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(m_writer.get_position()(2) - m_config.z_offset.value));
        config.set_key_value("current_extruder_id", new ConfigOptionInt((int)m_writer.tool()->id()));
        if (m_writer.tool_is_extruder()) {
            if (print.config().single_extruder_multi_material) {
                // Process the end_filament_gcode for the active filament only.
                int extruder_id = m_writer.tool()->id();
                config.set_key_value("filament_extruder_id", new ConfigOptionInt(extruder_id));
                _writeln(file, this->placeholder_parser_process("end_filament_gcode", print.config().end_filament_gcode.get_at(extruder_id), extruder_id, &config));
            } else {
                for (const std::string& end_gcode : print.config().end_filament_gcode.values) {
                    int extruder_id = (unsigned int)(&end_gcode - &print.config().end_filament_gcode.values.front());
                    config.set_key_value("filament_extruder_id", new ConfigOptionInt(extruder_id));
                    config.set_key_value("previous_extruder", new ConfigOptionInt(extruder_id));
                    config.set_key_value("next_extruder", new ConfigOptionInt(0));
                    _writeln(file, this->placeholder_parser_process("end_filament_gcode", end_gcode, extruder_id, &config));
                }
            }
        }
        _writeln(file, this->placeholder_parser_process("end_gcode", print.config().end_gcode, m_writer.tool()->id(), &config));
    }
    _write(file, m_writer.update_progress(m_layer_count, m_layer_count, true)); // 100%
    _write(file, m_writer.postamble());

    // adds tags for time estimators
    if (print.config().remaining_times.value)
    {
        _writeln(file, GCodeTimeEstimator::Normal_Last_M73_Output_Placeholder_Tag);
        if (m_silent_time_estimator_enabled)
            _writeln(file, GCodeTimeEstimator::Silent_Last_M73_Output_Placeholder_Tag);
    }

    print.throw_if_canceled();

    // calculates estimated printing time
    m_normal_time_estimator.calculate_time(false);
    if (m_silent_time_estimator_enabled)
        m_silent_time_estimator.calculate_time(false);
    if (config().time_estimation_compensation.get_abs_value(1) != 1) {
        m_normal_time_estimator.scale_time(config().time_estimation_compensation.get_abs_value(1));
        if (m_silent_time_estimator_enabled)
            m_silent_time_estimator.scale_time(config().time_estimation_compensation.get_abs_value(1));
    }

    // Get filament stats.
    _write(file, DoExport::update_print_stats_and_format_filament_stats(
    	// Const inputs
        m_normal_time_estimator, m_silent_time_estimator, m_silent_time_estimator_enabled, 
        has_wipe_tower, print.wipe_tower_data(), 
        m_writer.extruders(),
        // Modifies
        print.m_print_statistics));
    _write(file, "\n");
    _write_format(file, "; total filament used [g] = %.1lf\n", print.m_print_statistics.total_weight);
    _write_format(file, "; total filament cost = %.1lf\n", print.m_print_statistics.total_cost);
    if (print.m_print_statistics.total_toolchanges > 0)
    	_write_format(file, "; total toolchanges = %i\n", print.m_print_statistics.total_toolchanges);
    _write_format(file, "; estimated printing time (normal mode) = %s\n", m_normal_time_estimator.get_time_dhms().c_str());
    if (m_silent_time_estimator_enabled)
        _write_format(file, "; estimated printing time (silent mode) = %s\n", m_silent_time_estimator.get_time_dhms().c_str());

    // Append full config.
    _write(file, "\n");
    {
        std::string full_config = "";
        append_full_config(print, full_config);
        if (!full_config.empty())
            _write(file, full_config);
    }
    print.throw_if_canceled();
}

std::string GCode::placeholder_parser_process(const std::string &name, const std::string &templ, unsigned int current_extruder_id, const DynamicConfig *config_override)
{
    try {
        return m_placeholder_parser.process(templ, current_extruder_id, config_override);
    } catch (std::runtime_error &err) {
        // Collect the names of failed template substitutions for error reporting.
        m_placeholder_parser_failed_templates.insert(name);
        // Insert the macro error message into the G-code.
        return
            std::string("\n!!!!! Failed to process the custom G-code template ") + name + "\n" + 
            err.what() + 
            "!!!!! End of an error report for the custom G-code template " + name + "\n\n";
    }
}

// Parse the custom G-code, try to find mcode_set_temp_dont_wait and mcode_set_temp_and_wait or optionally G10 with temperature inside the custom G-code.
// Returns true if one of the temp commands are found, and try to parse the target temperature value into temp_out.
static bool custom_gcode_sets_temperature(const std::string &gcode, const int mcode_set_temp_dont_wait, const int mcode_set_temp_and_wait, const bool include_g10, int &temp_out)
{
    temp_out = -1;
    if (gcode.empty())
        return false;

    const char *ptr = gcode.data();
    bool temp_set_by_gcode = false;
    while (*ptr != 0) {
        // Skip whitespaces.
        for (; *ptr == ' ' || *ptr == '\t'; ++ ptr);
        if (*ptr == 'M') {
            // Line starts with 'M'. It is a machine command.
            ++ ptr;
            // Parse the M code value.
            char *endptr = nullptr;
            int mcode = int(strtol(ptr, &endptr, 10));
            if (endptr != nullptr && endptr != ptr && (mcode == mcode_set_temp_dont_wait || mcode == mcode_set_temp_and_wait)) {
                // M104/M109 or M140/M190 found.
				ptr = endptr;
                // Let the caller know that the custom G-code sets the temperature.
                temp_set_by_gcode = true;
                // Now try to parse the temperature value.
				// While not at the end of the line:
				while (strchr(";\r\n\0", *ptr) == nullptr) {
                    // Skip whitespaces.
                    for (; *ptr == ' ' || *ptr == '\t'; ++ ptr);
                    if (*ptr == 'S') {
                        // Skip whitespaces.
                        for (++ ptr; *ptr == ' ' || *ptr == '\t'; ++ ptr);
                        // Parse an int.
                        endptr = nullptr;
                        long temp_parsed = strtol(ptr, &endptr, 10);
						if (endptr > ptr) {
							ptr = endptr;
							temp_out = temp_parsed;
						}
                    } else {
                        // Skip this word.
						for (; strchr(" \t;\r\n\0", *ptr) == nullptr; ++ ptr);
                    }
                }
            }
        } else if (*ptr == 'G' && include_g10) { // Only check for G10 if requested
            // Line starts with 'G'.
            ++ ptr;
            // Parse the G code value.
            char *endptr = nullptr;
            int gcode = int(strtol(ptr, &endptr, 10));
            if (endptr != nullptr && endptr != ptr && gcode == 10 /* G10 */) {
                // G10 code found
                ptr = endptr;
                // Now try to parse the temperature value.
                // While not at the end of the line:
                while (strchr(";\r\n\0", *ptr) == nullptr) {
                    // Skip whitespaces.
                    for (; *ptr == ' ' || *ptr == '\t'; ++ ptr);
                    if (*ptr == 'S') {
                        // Skip whitespaces.
                        for (++ ptr; *ptr == ' ' || *ptr == '\t'; ++ ptr);
                        // Parse an int.
                        endptr = nullptr;
                        long temp_parsed = strtol(ptr, &endptr, 10);
                        if (endptr > ptr) {
                            ptr = endptr;
                            temp_out = temp_parsed;
                            // Let the caller know that the custom G-code sets the temperature
                            // Only do this after successfully parsing temperature since G10
                            // can be used for other reasons
                            temp_set_by_gcode = true;
                        }
                    } else {
                        // Skip this word.
                        for (; strchr(" \t;\r\n\0", *ptr) == nullptr; ++ ptr);
                    }
                }
            }
        }
        // Skip the rest of the line.
        for (; *ptr != 0 && *ptr != '\r' && *ptr != '\n'; ++ ptr);
		// Skip the end of line indicators.
        for (; *ptr == '\r' || *ptr == '\n'; ++ ptr);
	}
    return temp_set_by_gcode;
}

// Print the machine envelope G-code for the Marlin firmware based on the "machine_max_xxx" parameters.
// Do not process this piece of G-code by the time estimator, it already knows the values through another sources.
void GCode::print_machine_envelope(FILE *file, Print &print)
{
   // gcfRepRap, gcfRepetier, gcfTeacup, gcfMakerWare, gcfMarlin, gcfKlipper, gcfSailfish, gcfSprinter, gcfMach3, gcfMachinekit,
   ///     gcfSmoothie, gcfNoExtrusion, gcfLerdge,
    if (print.config().print_machine_envelope) {
        if (std::set<uint8_t>{gcfMarlin, gcfLerdge, gcfRepetier, gcfRepRap,  gcfSprinter}.count(print.config().gcode_flavor.value) > 0)
            fprintf(file, "M201 X%d Y%d Z%d E%d ; sets maximum accelerations, mm/sec^2\n",
                int(print.config().machine_max_acceleration_x.values.front() + 0.5),
                int(print.config().machine_max_acceleration_y.values.front() + 0.5),
                int(print.config().machine_max_acceleration_z.values.front() + 0.5),
                int(print.config().machine_max_acceleration_e.values.front() + 0.5));
        if (std::set<uint8_t>{gcfRepetier}.count(print.config().gcode_flavor.value) > 0)
            fprintf(file, "M202 X%d Y%d ; sets maximum travel speed\n",
                int(print.config().travel_speed.value),
                int(print.config().travel_speed.value));
        if (std::set<uint8_t>{gcfMarlin, gcfLerdge, gcfRepetier, gcfRepRap, gcfSmoothie, gcfSprinter}.count(print.config().gcode_flavor.value) > 0)
            fprintf(file, "M203 X%d Y%d Z%d E%d ; sets maximum feedrates, mm/sec\n",
                int(print.config().machine_max_feedrate_x.values.front() + 0.5),
                int(print.config().machine_max_feedrate_y.values.front() + 0.5),
                int(print.config().machine_max_feedrate_z.values.front() + 0.5),
                int(print.config().machine_max_feedrate_e.values.front() + 0.5));
        if (std::set<uint8_t>{gcfMarlin, gcfLerdge}.count(print.config().gcode_flavor.value) > 0)
            fprintf(file, "M204 P%d R%d T%d ; sets acceleration (P, T) and retract acceleration (R), mm/sec^2\n",
                int(print.config().machine_max_acceleration_extruding.values.front() + 0.5),
                int(print.config().machine_max_acceleration_retracting.values.front() + 0.5),
                int(print.config().machine_max_acceleration_travel.values.front() + 0.5));
        if (std::set<uint8_t>{gcfRepRap, gcfKlipper, gcfSprinter}.count(print.config().gcode_flavor.value) > 0)
            fprintf(file, "M204 P%d T%d ; sets acceleration (P, T), mm/sec^2\n",
                int(print.config().machine_max_acceleration_extruding.values.front() + 0.5),
                int(print.config().machine_max_acceleration_travel.values.front() + 0.5));
        if (std::set<uint8_t>{gcfRepRap}.count(print.config().gcode_flavor.value) > 0)
            fprintf(file, "M566 X%.2lf Y%.2lf Z%.2lf E%.2lf ; sets the jerk limits, mm/sec\n",
                print.config().machine_max_jerk_x.values.front(),
                print.config().machine_max_jerk_y.values.front(),
                print.config().machine_max_jerk_z.values.front(),
                print.config().machine_max_jerk_e.values.front());
        if (std::set<uint8_t>{gcfMarlin, gcfLerdge, gcfRepetier}.count(print.config().gcode_flavor.value) > 0)
            fprintf(file, "M205 X%.2lf Y%.2lf Z%.2lf E%.2lf ; sets the jerk limits, mm/sec\n",
                print.config().machine_max_jerk_x.values.front(),
                print.config().machine_max_jerk_y.values.front(),
                print.config().machine_max_jerk_z.values.front(),
                print.config().machine_max_jerk_e.values.front());
        if (std::set<uint8_t>{gcfSmoothie}.count(print.config().gcode_flavor.value) > 0)
            fprintf(file, "M205 X%.2lf Z%.2lf ; sets the jerk limits, mm/sec\n",
                std::min(print.config().machine_max_jerk_x.values.front(),
                print.config().machine_max_jerk_y.values.front()),
                print.config().machine_max_jerk_z.values.front());
        if (std::set<uint8_t>{gcfMarlin, gcfLerdge, gcfRepetier}.count(print.config().gcode_flavor.value) > 0)
            fprintf(file, "M205 S%d T%d ; sets the minimum extruding and travel feed rate, mm/sec\n",
                int(print.config().machine_min_extruding_rate.values.front() + 0.5),
                int(print.config().machine_min_travel_rate.values.front() + 0.5));
    }
}

// Write 1st layer bed temperatures into the G-code.
// Only do that if the start G-code does not already contain any M-code controlling an extruder temperature.
// M140 - Set Bed Temperature
// M190 - Set Bed Temperature and Wait
void GCode::_print_first_layer_bed_temperature(FILE *file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait)
{
    // Initial bed temperature based on the first extruder.
    int  temp = print.config().first_layer_bed_temperature.get_at(first_printing_extruder_id);
    // Is the bed temperature set by the provided custom G-code?
    int  temp_by_gcode     = -1;
    bool temp_set_by_gcode = custom_gcode_sets_temperature(gcode, 140, 190, false, temp_by_gcode);
    if (temp_set_by_gcode && temp_by_gcode >= 0 && temp_by_gcode < 1000)
        temp = temp_by_gcode;
    // Always call m_writer.set_bed_temperature() so it will set the internal "current" state of the bed temp as if
    // the custom start G-code emited these.
    std::string set_temp_gcode = m_writer.set_bed_temperature(temp, wait);
    if ( !temp_set_by_gcode && this->config().gcode_flavor != gcfKlipper)
        _write(file, set_temp_gcode);
}

// Write 1st layer extruder temperatures into the G-code.
// Only do that if the start G-code does not already contain any M-code controlling an extruder temperature.
// M104 - Set Extruder Temperature
// M109 - Set Extruder Temperature and Wait
void GCode::_print_first_layer_extruder_temperatures(FILE *file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait)
{
    // Is the bed temperature set by the provided custom G-code?
    int  temp_by_gcode     = -1;
    if (custom_gcode_sets_temperature(gcode, 104, 109, true, temp_by_gcode)) {
        // Set the extruder temperature at m_writer, but throw away the generated G-code as it will be written with the custom G-code.
        int temp = print.config().first_layer_temperature.get_at(first_printing_extruder_id);
        if (temp_by_gcode >= 0 && temp_by_gcode < 1000)
            temp = temp_by_gcode;
        m_writer.set_temperature(temp, wait, first_printing_extruder_id);
    } else if(this->config().gcode_flavor != gcfKlipper){
        // Custom G-code does not set the extruder temperature. Do it now.
        if (print.config().single_extruder_multi_material.value) {
            // Set temperature of the first printing extruder only.
            int temp = print.config().first_layer_temperature.get_at(first_printing_extruder_id);
            if (temp > 0)
                _write(file, m_writer.set_temperature(temp, wait, first_printing_extruder_id));
        } else {
            // Set temperatures of all the printing extruders.
            for (unsigned int tool_id : print.extruders()) {
                int temp = print.config().first_layer_temperature.get_at(tool_id);
                if (print.config().ooze_prevention.value)
                    temp += print.config().standby_temperature_delta.value;
                if (temp > 0)
                    _write(file, m_writer.set_temperature(temp, wait, tool_id));
            }
        }
    }
}

inline GCode::ObjectByExtruder& object_by_extruder(
    std::map<unsigned int, std::vector<GCode::ObjectByExtruder>> &by_extruder, 
    unsigned int                                                  extruder_id, 
    size_t                                                        object_idx, 
    size_t                                                        num_objects)
{
    std::vector<GCode::ObjectByExtruder> &objects_by_extruder = by_extruder[extruder_id];
    if (objects_by_extruder.empty())
        objects_by_extruder.assign(num_objects, GCode::ObjectByExtruder());
    return objects_by_extruder[object_idx];
}

inline std::vector<GCode::ObjectByExtruder::Island>& object_islands_by_extruder(
    std::map<unsigned int, std::vector<GCode::ObjectByExtruder>>  &by_extruder, 
    unsigned int                                                   extruder_id, 
    size_t                                                         object_idx, 
    size_t                                                         num_objects,
    size_t                                                         num_islands)
{
    std::vector<GCode::ObjectByExtruder::Island> &islands = object_by_extruder(by_extruder, extruder_id, object_idx, num_objects).islands;
    if (islands.empty())
        islands.assign(num_islands, GCode::ObjectByExtruder::Island());
    return islands;
}

std::vector<GCode::InstanceToPrint> GCode::sort_print_object_instances(
	std::vector<GCode::ObjectByExtruder> 		&objects_by_extruder,
	const std::vector<LayerToPrint> 			&layers,
	// Ordering must be defined for normal (non-sequential print).
	const std::vector<const PrintInstance*> 	*ordering,
	// For sequential print, the instance of the object to be printing has to be defined.
	const size_t                     		 	 single_object_instance_idx)
{
    std::vector<InstanceToPrint> out;

    if (ordering == nullptr) {
    	// Sequential print, single object is being printed.
		for (ObjectByExtruder &object_by_extruder : objects_by_extruder) {
		    const size_t       layer_id     = &object_by_extruder - objects_by_extruder.data();
		    const PrintObject *print_object = layers[layer_id].object();
		    if (print_object)
		    	out.emplace_back(object_by_extruder, layer_id, *print_object, single_object_instance_idx);
		}
    } else {
		// Create mapping from PrintObject* to ObjectByExtruder*.
		std::vector<std::pair<const PrintObject*, ObjectByExtruder*>> sorted;
		sorted.reserve(objects_by_extruder.size());
		for (ObjectByExtruder &object_by_extruder : objects_by_extruder) {
		    const size_t       layer_id     = &object_by_extruder - objects_by_extruder.data();
		    const PrintObject *print_object = layers[layer_id].object();
		    if (print_object)
		    	sorted.emplace_back(print_object, &object_by_extruder);
		}
		std::sort(sorted.begin(), sorted.end());

		if (! sorted.empty()) {
			const Print &print = *sorted.front().first->print();
		    out.reserve(sorted.size());
		    for (const PrintInstance *instance : *ordering) {
		    	const PrintObject &print_object = *instance->print_object;
		    	std::pair<const PrintObject*, ObjectByExtruder*> key(&print_object, nullptr);
		    	auto it = std::lower_bound(sorted.begin(), sorted.end(), key);
		    	if (it != sorted.end() && it->first == &print_object)
		    		// ObjectByExtruder for this PrintObject was found.
					out.emplace_back(*it->second, it->second - objects_by_extruder.data(), print_object, instance - print_object.instances().data());
		    }
		}
	}
	return out;
}

std::string GCode::emit_custom_gcode_per_print_z(
    const CustomGCode::Item                                 *custom_gcode,
    // ID of the first extruder printing this layer.
    unsigned int                                            first_extruder_id,
    const Print                                             &print,
    PrintStatistics                                         &stats)
{
    bool single_extruder_printer = print.config().nozzle_diameter.size() == 1;
    std::string gcode;
        
    if (custom_gcode != nullptr) {
        // Extruder switches are processed by LayerTools, they should be filtered out.
        assert(custom_gcode->gcode != ToolChangeCode);

        const std::string  &custom_code  = custom_gcode->gcode;
        bool                color_change = custom_code == ColorChangeCode;
        bool                tool_change  = custom_code == ToolChangeCode;
        // Tool Change is applied as Color Change for a single extruder printer only.
        assert(! tool_change || single_extruder_printer);

        std::string pause_print_msg;
        int m600_extruder_before_layer = -1;
        if (color_change && custom_gcode->extruder > 0)
            m600_extruder_before_layer = custom_gcode->extruder - 1;
        else if (custom_code == PausePrintCode)
            pause_print_msg = custom_gcode->color;

        if (color_change) {
            //update stats : weight
            double previously_extruded = 0;
            for (const auto& tuple : stats.color_extruderid_to_used_weight)
                if (tuple.first == this->m_writer.tool()->id())
                    previously_extruded += tuple.second;
            double extruded = this->m_writer.tool()->filament_density() * this->m_writer.tool()->extruded_volume();
            stats.color_extruderid_to_used_weight.emplace_back(this->m_writer.tool()->id(), extruded - previously_extruded);

            //update stats : length
            previously_extruded = 0;
            for (const auto& tuple : stats.color_extruderid_to_used_filament)
                if (tuple.first == this->m_writer.tool()->id())
                    previously_extruded += tuple.second;
            stats.color_extruderid_to_used_filament.emplace_back(this->m_writer.tool()->id(), this->m_writer.tool()->used_filament() - previously_extruded);
        }

        // we should add or not colorprint_change in respect to nozzle_diameter count instead of really used extruders count
        if (color_change || tool_change)
        {

            // Color Change or Tool Change as Color Change.
            // add tag for analyzer
            gcode += "; " + GCodeAnalyzer::Color_Change_Tag + ",T" + std::to_string(m600_extruder_before_layer) + "\n";
            // add tag for time estimator
            gcode += "; " + GCodeTimeEstimator::Color_Change_Tag + "\n";

            if (!single_extruder_printer && m600_extruder_before_layer >= 0 && first_extruder_id != m600_extruder_before_layer
                // && !MMU1
                ) {
                //! FIXME_in_fw show message during print pause
                gcode += "M601\n"; // pause print
                gcode += "M117 Change filament for Extruder " + std::to_string(m600_extruder_before_layer) + "\n";
            }
            else {
                gcode += ColorChangeCode;
                gcode += "\n";
            }
        } 
        else
        {
            if (custom_code == PausePrintCode) // Pause print
            {
                // add tag for analyzer
                gcode += "; " + GCodeAnalyzer::Pause_Print_Tag + "\n";
                //! FIXME_in_fw show message during print pause
                if (!pause_print_msg.empty())
                    gcode += "M117 " + pause_print_msg + "\n";
                // add tag for time estimator
                gcode += "; " + GCodeTimeEstimator::Pause_Print_Tag + "\n";
            }
            else // custom Gcode
            {
                // add tag for analyzer
                gcode += "; " + GCodeAnalyzer::Custom_Code_Tag + "\n";
                // add tag for time estimator
                //gcode += "; " + GCodeTimeEstimator::Custom_Code_Tag + "\n";
            }
            gcode += custom_code + "\n";
        }
    }

    return gcode;
}

namespace Skirt {
	static void skirt_loops_per_extruder_all_printing(const Print &print, const LayerTools &layer_tools, std::map<unsigned int, std::pair<size_t, size_t>> &skirt_loops_per_extruder_out)
	{
        // Prime all extruders printing over the 1st layer over the skirt lines.
        size_t n_loops = print.skirt().entities.size();
        size_t n_tools = layer_tools.extruders.size();
        size_t lines_per_extruder = (n_loops + n_tools - 1) / n_tools;
        for (size_t i = 0; i < n_loops; i += lines_per_extruder)
            skirt_loops_per_extruder_out[layer_tools.extruders[i / lines_per_extruder]] = std::pair<size_t, size_t>(i, std::min(i + lines_per_extruder, n_loops));
	}

    static std::map<unsigned int, std::pair<size_t, size_t>> make_skirt_loops_per_extruder_1st_layer(
        const Print             				&print,
	    const std::vector<GCode::LayerToPrint> 	& /*layers */,
	    const LayerTools                		&layer_tools,
        // Heights (print_z) at which the skirt has already been extruded.
        std::vector<coordf_t>  			    	&skirt_done)
    {
        // Extrude skirt at the print_z of the raft layers and normal object layers
        // not at the print_z of the interlaced support material layers.
        std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder_out;
        if (skirt_done.empty() && print.has_skirt() && ! print.skirt().entities.empty()) {
        	skirt_loops_per_extruder_all_printing(print, layer_tools, skirt_loops_per_extruder_out);
            skirt_done.emplace_back(layer_tools.print_z);
        }
        return skirt_loops_per_extruder_out;
    }

    static std::map<unsigned int, std::pair<size_t, size_t>> make_skirt_loops_per_extruder_other_layers(
        const Print 							&print,
	    const std::vector<GCode::LayerToPrint> 	&layers,
	    const LayerTools                		&layer_tools,
	    // First non-empty support layer.
	    const SupportLayer  					*support_layer,
	    // Heights (print_z) at which the skirt has already been extruded.
        std::vector<coordf_t>			    	&skirt_done)
    {
        // Extrude skirt at the print_z of the raft layers and normal object layers
        // not at the print_z of the interlaced support material layers.
        std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder_out;
        if (print.has_skirt() && ! print.skirt().entities.empty() &&
            // Not enough skirt layers printed yet.
            //FIXME infinite or high skirt does not make sense for sequential print!
            (skirt_done.size() < (size_t)print.config().skirt_height.value || print.has_infinite_skirt()) &&
            // This print_z has not been extruded yet (sequential print)
            skirt_done.back() < layer_tools.print_z - EPSILON &&
            // and this layer is an object layer, or it is a raft layer.
            (layer_tools.has_object || support_layer->id() < (size_t)support_layer->object()->config().raft_layers.value)) {
#if 0
            // Prime just the first printing extruder. This is original Slic3r's implementation.
            skirt_loops_per_extruder_out[layer_tools.extruders.front()] = std::pair<size_t, size_t>(0, print.config().skirts.value);
#else
            // Prime all extruders planned for this layer, see 
            // https://github.com/prusa3d/PrusaSlicer/issues/469#issuecomment-322450619
            skirt_loops_per_extruder_all_printing(print, layer_tools, skirt_loops_per_extruder_out);
#endif
            assert(!skirt_done.empty());
            skirt_done.emplace_back(layer_tools.print_z);
        }
        return skirt_loops_per_extruder_out;
    }

} // namespace Skirt

// In sequential mode, process_layer is called once per each object and its copy, 
// therefore layers will contain a single entry and single_object_instance_idx will point to the copy of the object.
// In non-sequential mode, process_layer is called per each print_z height with all object and support layers accumulated.
// For multi-material prints, this routine minimizes extruder switches by gathering extruder specific extrusion paths
// and performing the extruder specific extrusions together.
void GCode::process_layer(
    // Write into the output file.
    FILE                                    *file,
    const Print                             &print,
    PrintStatistics                         &print_stat,
    // Set of object & print layers of the same PrintObject and with the same print_z.
    const std::vector<LayerToPrint>         &layers,
    const LayerTools                        &layer_tools,
	// Pairs of PrintObject index and its instance index.
	const std::vector<const PrintInstance*> *ordering,
    // If set to size_t(-1), then print all copies of all objects.
    // Otherwise print a single copy of a single object.
    const size_t                     		 single_object_instance_idx)
{
    assert(! layers.empty());
//    assert(! layer_tools.extruders.empty());
    // Either printing all copies of all objects, or just a single copy of a single object.
    assert(single_object_instance_idx == size_t(-1) || layers.size() == 1);

    if (layer_tools.extruders.empty())
        // Nothing to extrude.
        return;

    // Extract 1st object_layer and support_layer of this set of layers with an equal print_z.
    const Layer         *object_layer  = nullptr;
    const SupportLayer  *support_layer = nullptr;
    for (const LayerToPrint &l : layers) {
        if (l.object_layer != nullptr && object_layer == nullptr)
            object_layer = l.object_layer;
        if (l.support_layer != nullptr && support_layer == nullptr)
            support_layer = l.support_layer;
    }
    const Layer         &layer         = (object_layer != nullptr) ? *object_layer : *support_layer;
    coordf_t             print_z       = layer.print_z;
    bool                 first_layer   = layer.id() == 0;
    unsigned int         first_extruder_id = layer_tools.extruders.front();

    // Initialize config with the 1st object to be printed at this layer.
    m_config.apply(layer.object()->config(), true);

    // Check whether it is possible to apply the spiral vase logic for this layer.
    // Just a reminder: A spiral vase mode is allowed for a single object, single material print only.
    if (m_spiral_vase && layers.size() == 1 && support_layer == nullptr) {
        bool enable = (layer.id() > 0 || layer.object()->config().brim_width.value == 0.) && (layer.id() >= (size_t)print.config().skirt_height.value && ! print.has_infinite_skirt());
        if (enable) {
            for (const LayerRegion *layer_region : layer.regions())
                if (size_t(layer_region->region()->config().bottom_solid_layers.value) > layer.id() ||
                    layer_region->perimeters.items_count() > 1u ||
                    layer_region->fills.items_count() > 0) {
                    enable = false;
                    break;
                }
        }
        m_spiral_vase->enable = enable;
    }
    // If we're going to apply spiralvase to this layer, disable loop clipping
    m_enable_loop_clipping = !m_spiral_vase || !m_spiral_vase->enable;
    
    std::string gcode;

    // Set new layer - this will change Z and force a retraction if retract_layer_change is enabled.
    coordf_t previous_print_z = m_layer != nullptr ? m_layer->print_z : 0;
    if (! print.config().before_layer_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("previous_layer_z", new ConfigOptionFloat(previous_print_z));
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index + 1));
        config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
        gcode += this->placeholder_parser_process("before_layer_gcode",
            print.config().before_layer_gcode.value, m_writer.tool()->id(), &config)
            + "\n";
    }
    gcode += this->change_layer(print_z);  // this will increase m_layer_index
	m_layer = &layer;
    if (! print.config().layer_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("previous_layer_z", new ConfigOptionFloat(previous_print_z));
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z",   new ConfigOptionFloat(print_z));
        gcode += this->placeholder_parser_process("layer_gcode",
            print.config().layer_gcode.value, m_writer.tool()->id(), &config)
            + "\n";
    }

    if (! first_layer && ! m_second_layer_things_done) {
        // Transition from 1st to 2nd layer. Adjust nozzle temperatures as prescribed by the nozzle dependent
        // first_layer_temperature vs. temperature settings.
        for (const Extruder &extruder : m_writer.extruders()) {
            if (print.config().single_extruder_multi_material.value && extruder.id() != m_writer.tool()->id())
                // In single extruder multi material mode, set the temperature for the current extruder only.
                continue;
            int temperature = print.config().temperature.get_at(extruder.id());
            if (temperature > 0 && temperature != print.config().first_layer_temperature.get_at(extruder.id()))
                gcode += m_writer.set_temperature(temperature, false, extruder.id());
        }
        gcode += m_writer.set_bed_temperature(print.config().bed_temperature.get_at(first_extruder_id));
        // Mark the temperature transition from 1st to 2nd layer to be finished.
        m_second_layer_things_done = true;
    }

    // Map from extruder ID to <begin, end> index of skirt loops to be extruded with that extruder.
    std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder;

    if (single_object_instance_idx == size_t(-1)) {
        // Normal (non-sequential) print.
        gcode += this->emit_custom_gcode_per_print_z(layer_tools.custom_gcode, first_extruder_id, print, print_stat);
    }
    // Extrude skirt at the print_z of the raft layers and normal object layers
    // not at the print_z of the interlaced support material layers.
    skirt_loops_per_extruder = first_layer ?
        Skirt::make_skirt_loops_per_extruder_1st_layer(print, layers, layer_tools, m_skirt_done) :
        Skirt::make_skirt_loops_per_extruder_other_layers(print, layers, layer_tools, support_layer, m_skirt_done);

    // Group extrusions by an extruder, then by an object, an island and a region.
    std::map<unsigned int, std::vector<ObjectByExtruder>> by_extruder;
    bool is_anything_overridden = const_cast<LayerTools&>(layer_tools).wiping_extrusions().is_anything_overridden();
    for (const LayerToPrint &layer_to_print : layers) {
        if (layer_to_print.support_layer != nullptr) {
            const SupportLayer &support_layer = *layer_to_print.support_layer;
            const PrintObject  &object = *support_layer.object();
            if (! support_layer.support_fills.entities.empty()) {
                ExtrusionRole   role               = support_layer.support_fills.role();
                bool            has_support        = role == erMixed || role == erSupportMaterial;
                bool            has_interface      = role == erMixed || role == erSupportMaterialInterface;
                // Extruder ID of the support base. -1 if "don't care".
                unsigned int    support_extruder   = object.config().support_material_extruder.value - 1;
                // Shall the support be printed with the active extruder, preferably with non-soluble, to avoid tool changes?
                bool            support_dontcare   = object.config().support_material_extruder.value == 0;
                // Extruder ID of the support interface. -1 if "don't care".
                unsigned int    interface_extruder = object.config().support_material_interface_extruder.value - 1;
                // Shall the support interface be printed with the active extruder, preferably with non-soluble, to avoid tool changes?
                bool            interface_dontcare = object.config().support_material_interface_extruder.value == 0;
                if (support_dontcare || interface_dontcare) {
                    // Some support will be printed with "don't care" material, preferably non-soluble.
                    // Is the current extruder assigned a soluble filament?
                    unsigned int dontcare_extruder = first_extruder_id;
                    if (print.config().filament_soluble.get_at(dontcare_extruder)) {
                        // The last extruder printed on the previous layer extrudes soluble filament.
                        // Try to find a non-soluble extruder on the same layer.
                        for (unsigned int extruder_id : layer_tools.extruders)
                            if (! print.config().filament_soluble.get_at(extruder_id)) {
                                dontcare_extruder = extruder_id;
                                break;
                            }
                    }
                    if (support_dontcare)
                        support_extruder = dontcare_extruder;
                    if (interface_dontcare)
                        interface_extruder = dontcare_extruder;
                }
                // Both the support and the support interface are printed with the same extruder, therefore
                // the interface may be interleaved with the support base.
                bool single_extruder = ! has_support || support_extruder == interface_extruder;
                // Assign an extruder to the base.
                ObjectByExtruder &obj = object_by_extruder(by_extruder, has_support ? support_extruder : interface_extruder, &layer_to_print - layers.data(), layers.size());
                obj.support = &support_layer.support_fills;
                obj.support_extrusion_role = single_extruder ? erMixed : erSupportMaterial;
                if (! single_extruder && has_interface) {
                    ObjectByExtruder &obj_interface = object_by_extruder(by_extruder, interface_extruder, &layer_to_print - layers.data(), layers.size());
                    obj_interface.support = &support_layer.support_fills;
                    obj_interface.support_extrusion_role = erSupportMaterialInterface;
                }
            }
        }
        if (layer_to_print.object_layer != nullptr) {
            const Layer &layer = *layer_to_print.object_layer;
            // We now define a strategy for building perimeters and fills. The separation 
            // between regions doesn't matter in terms of printing order, as we follow 
            // another logic instead:
            // - we group all extrusions by extruder so that we minimize toolchanges
            // - we start from the last used extruder
            // - for each extruder, we group extrusions by island
            // - for each island, we extrude perimeters first, unless user set the infill_first
            //   option
            // (Still, we have to keep track of regions because we need to apply their config)
            size_t n_slices = layer.lslices.size();
            const std::vector<BoundingBox> &layer_surface_bboxes = layer.lslices_bboxes;
            // Traverse the slices in an increasing order of bounding box size, so that the islands inside another islands are tested first,
            // so we can just test a point inside ExPolygon::contour and we may skip testing the holes.
            std::vector<size_t> slices_test_order;
            slices_test_order.reserve(n_slices);
            for (size_t i = 0; i < n_slices; ++ i)
            	slices_test_order.emplace_back(i);
            std::sort(slices_test_order.begin(), slices_test_order.end(), [&layer_surface_bboxes](size_t i, size_t j) {
            	const Vec2d s1 = layer_surface_bboxes[i].size().cast<double>();
            	const Vec2d s2 = layer_surface_bboxes[j].size().cast<double>();
            	return s1.x() * s1.y() < s2.x() * s2.y();
            });
            auto point_inside_surface = [&layer, &layer_surface_bboxes](const size_t i, const Point &point) { 
                const BoundingBox &bbox = layer_surface_bboxes[i];
                return point(0) >= bbox.min(0) && point(0) < bbox.max(0) &&
                       point(1) >= bbox.min(1) && point(1) < bbox.max(1) &&
                       layer.lslices[i].contour.contains(point);
            };

            for (size_t region_id = 0; region_id < layer.regions().size(); ++ region_id) {
                const LayerRegion *layerm = layer.regions()[region_id];
                if (layerm == nullptr)
                    continue;
                const PrintRegion &region = *print.regions()[region_id];

                // Now we must process perimeters and infills and create islands of extrusions in by_region std::map.
                // It is also necessary to save which extrusions are part of MM wiping and which are not.
                // The process is almost the same for perimeters and infills - we will do it in a cycle that repeats twice:
                std::vector<unsigned int> printing_extruders;
                for (const ObjectByExtruder::Island::Region::Type entity_type : { ObjectByExtruder::Island::Region::INFILL, ObjectByExtruder::Island::Region::PERIMETERS }) {
                    for (const ExtrusionEntity *ee : (entity_type == ObjectByExtruder::Island::Region::INFILL) ? layerm->fills.entities : layerm->perimeters.entities) {
                        // extrusions represents infill or perimeter extrusions of a single island.
                        assert(dynamic_cast<const ExtrusionEntityCollection*>(ee) != nullptr);
                        const auto *extrusions = static_cast<const ExtrusionEntityCollection*>(ee);
                        if (extrusions->entities.empty()) // This shouldn't happen but first_point() would fail.
                            continue;

                        // This extrusion is part of certain Region, which tells us which extruder should be used for it:
                        int correct_extruder_id = layer_tools.extruder(*extrusions, region);

                        // Let's recover vector of extruder overrides:
                        const WipingExtrusions::ExtruderPerCopy *entity_overrides = nullptr;
                        if (! layer_tools.has_extruder(correct_extruder_id)) {
							// this entity is not overridden, but its extruder is not in layer_tools - we'll print it
                            // by last extruder on this layer (could happen e.g. when a wiping object is taller than others - dontcare extruders are eradicated from layer_tools)
                            correct_extruder_id = layer_tools.extruders.back();
                        }
                        printing_extruders.clear();
                        if (is_anything_overridden) {
                        	entity_overrides = const_cast<LayerTools&>(layer_tools).wiping_extrusions().get_extruder_overrides(extrusions, correct_extruder_id, layer_to_print.object()->instances().size());
	                        if (entity_overrides == nullptr) {
		                    	printing_extruders.emplace_back(correct_extruder_id);
	                        } else {
	                        	printing_extruders.reserve(entity_overrides->size());
	                        	for (int extruder : *entity_overrides)
	                        		printing_extruders.emplace_back(extruder >= 0 ? 
	                        			// at least one copy is overridden to use this extruder
	                        			extruder : 
	                        			// at least one copy would normally be printed with this extruder (see get_extruder_overrides function for explanation)
	                        			static_cast<unsigned int>(- extruder - 1));
		                        Slic3r::sort_remove_duplicates(printing_extruders);
	                        }
	                    } else
	                    	printing_extruders.emplace_back(correct_extruder_id);

                        // Now we must add this extrusion into the by_extruder map, once for each extruder that will print it:
                        for (unsigned int extruder : printing_extruders)
                        {
                            std::vector<ObjectByExtruder::Island> &islands = object_islands_by_extruder(
                                by_extruder,
                                extruder,
                                &layer_to_print - layers.data(),
                                layers.size(), n_slices+1);
                            for (size_t i = 0; i <= n_slices; ++ i) {
								bool   last = i == n_slices;
                            	size_t island_idx = last ? n_slices : slices_test_order[i];
                                if (// extrusions->first_point does not fit inside any slice
									last ||
                                    // extrusions->first_point fits inside ith slice
                                    point_inside_surface(island_idx, extrusions->first_point())) {
                                    if (islands[island_idx].by_region.empty())
                                        islands[island_idx].by_region.assign(print.regions().size(), ObjectByExtruder::Island::Region());
                                    islands[island_idx].by_region[region_id].append(entity_type, extrusions, entity_overrides);
                                    break;
                                }
                            }
                        }
                    }
                }
            } // for regions
        }
    } // for objects

    // Extrude the skirt, brim, support, perimeters, infill ordered by the extruders.
    std::vector<std::unique_ptr<EdgeGrid::Grid>> lower_layer_edge_grids(layers.size());
    for (unsigned int extruder_id : layer_tools.extruders)
    {
        gcode += (layer_tools.has_wipe_tower && m_wipe_tower) ?
            m_wipe_tower->tool_change(*this, extruder_id, extruder_id == layer_tools.extruders.back()) :
            this->set_extruder(extruder_id, print_z);

        // let analyzer tag generator aware of a role type change
        if (m_enable_analyzer && layer_tools.has_wipe_tower && m_wipe_tower)
            m_last_analyzer_extrusion_role = erWipeTower;

        //if first layer, ask for a bigger lift for travel to object, to be on the safe side
        set_extra_lift(layer, print, m_writer, extruder_id);

        if (auto loops_it = skirt_loops_per_extruder.find(extruder_id); loops_it != skirt_loops_per_extruder.end()) {
            const std::pair<size_t, size_t> loops = loops_it->second;
            this->set_origin(0., 0.);
            m_avoid_crossing_perimeters.use_external_mp = true;
            Flow layer_skirt_flow(print.skirt_flow(extruder_id));
            layer_skirt_flow.height = float(m_skirt_done.back() - (m_skirt_done.size() == 1 ? 0. : m_skirt_done[m_skirt_done.size() - 2]));
            double mm3_per_mm = layer_skirt_flow.mm3_per_mm();
            for (size_t i = loops.first; i < loops.second; ++i) {
                // Adjust flow according to this layer's layer height.
                ExtrusionLoop loop = *dynamic_cast<const ExtrusionLoop*>(print.skirt().entities[i]);
                for (ExtrusionPath &path : loop.paths) {
                    path.height = layer_skirt_flow.height;
                    path.mm3_per_mm = mm3_per_mm;
                }
                //FIXME using the support_material_speed of the 1st object printed.
                gcode += this->extrude_loop(loop, "skirt", m_config.support_material_speed.value);
            }
            m_avoid_crossing_perimeters.use_external_mp = false;
            // Allow a straight travel move to the first object point if this is the first layer (but don't in next layers).
            if (first_layer && loops.first == 0)
                m_avoid_crossing_perimeters.disable_once = true;
        }

        // Extrude brim with the extruder of the 1st region.
        if (! m_brim_done) {
            //if first layer, ask for a bigger lift for travel to object, to be on the safe side
            set_extra_lift(layer, print, m_writer, extruder_id);

            this->set_origin(0., 0.);
            m_avoid_crossing_perimeters.use_external_mp = true;
            gcode += this->extrude_entity(print.brim(), "brim", m_config.support_material_speed.value);
            m_brim_done = true;
            m_avoid_crossing_perimeters.use_external_mp = false;
            // Allow a straight travel move to the first object point.
            m_avoid_crossing_perimeters.disable_once = true;
        }
        //extrude object-only skirt
        if (single_object_instance_idx != size_t(-1) && !layers.front().object()->skirt().empty()
            && extruder_id == layer_tools.extruders.front()) {
            //if first layer, ask for a bigger lift for travel to object, to be on the safe side
            set_extra_lift(layer, print, m_writer, extruder_id);

            const PrintObject *print_object = layers.front().object();
            this->set_origin(unscale(print_object->instances()[single_object_instance_idx].shift));
            if (this->m_layer != nullptr && this->m_layer->id() < m_config.skirt_height) {
                for (const ExtrusionEntity *ee : print_object->skirt().entities)
                    gcode += this->extrude_entity(*ee, "skirt", m_config.support_material_speed.value);
            }
        }
        //extrude object-only brim
        if (single_object_instance_idx != size_t(-1) && !layers.front().object()->brim().empty()
            && extruder_id == layer_tools.extruders.front()) {
            //if first layer, ask for a bigger lift for travel to object, to be on the safe side
            set_extra_lift(layer, print, m_writer, extruder_id);

            const PrintObject *print_object = layers.front().object();
            this->set_origin(unscale(print_object->instances()[single_object_instance_idx].shift));
            if (this->m_layer != nullptr && this->m_layer->id() == 0) {
                m_avoid_crossing_perimeters.use_external_mp = true;
                for (const ExtrusionEntity *ee : print_object->brim().entities)
                    gcode += this->extrude_entity(*ee, "brim", m_config.support_material_speed.value);
                m_avoid_crossing_perimeters.use_external_mp = false;
                m_avoid_crossing_perimeters.disable_once = true;
            }
        }


        auto objects_by_extruder_it = by_extruder.find(extruder_id);
        if (objects_by_extruder_it == by_extruder.end())
            continue;

		std::vector<InstanceToPrint> instances_to_print = sort_print_object_instances(objects_by_extruder_it->second, layers, ordering, single_object_instance_idx);

        // We are almost ready to print. However, we must go through all the objects twice to print the the overridden extrusions first (infill/perimeter wiping feature):
		std::vector<ObjectByExtruder::Island::Region> by_region_per_copy_cache;
        for (int print_wipe_extrusions = is_anything_overridden; print_wipe_extrusions>=0; --print_wipe_extrusions) {
            if (is_anything_overridden && print_wipe_extrusions == 0)
                gcode+="; PURGING FINISHED\n";

            for (InstanceToPrint &instance_to_print : instances_to_print) {
                m_config.apply(instance_to_print.print_object.config(), true);
                m_layer = layers[instance_to_print.layer_id].layer();
                if (m_config.avoid_crossing_perimeters)
                    m_avoid_crossing_perimeters.init_layer_mp(union_ex(m_layer->lslices, true));

                if (this->config().gcode_label_objects)
                    gcode += std::string("; printing object ") + instance_to_print.print_object.model_object()->name 
                        + " id:" + std::to_string(std::find(this->m_ordered_objects.begin(), this->m_ordered_objects.end(), &instance_to_print.print_object) - this->m_ordered_objects.begin()) 
                        + " copy " + std::to_string(instance_to_print.instance_id) + "\n";
                //if first layer, ask for a bigger lift for travel to object, to be on the safe side
                set_extra_lift(layer, print, m_writer, extruder_id);
                // When starting a new object, use the external motion planner for the first travel move.
                const Point &offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                std::pair<const PrintObject*, Point> this_object_copy(&instance_to_print.print_object, offset);
                if (m_last_obj_copy != this_object_copy)
                    m_avoid_crossing_perimeters.use_external_mp_once = true;
                m_last_obj_copy = this_object_copy;
                this->set_origin(unscale(offset));
                if (instance_to_print.object_by_extruder.support != nullptr && !print_wipe_extrusions) {
                    m_layer = layers[instance_to_print.layer_id].support_layer;
                        gcode += this->extrude_support(
                            // support_extrusion_role is erSupportMaterial, erSupportMaterialInterface or erMixed for all extrusion paths.
                        instance_to_print.object_by_extruder.support->chained_path_from(m_last_pos, instance_to_print.object_by_extruder.support_extrusion_role));
                    m_layer = layers[instance_to_print.layer_id].layer();
                }
                for (ObjectByExtruder::Island &island : instance_to_print.object_by_extruder.islands) {
                    const auto& by_region_specific = 
                        is_anything_overridden ? 
                        island.by_region_per_copy(by_region_per_copy_cache, 
                            static_cast<unsigned int>(instance_to_print.instance_id), 
                            extruder_id, 
                            print_wipe_extrusions != 0) : 
                        island.by_region;
                    gcode += this->extrude_infill(print, by_region_specific, true);
                    gcode += this->extrude_perimeters(print, by_region_specific, lower_layer_edge_grids[instance_to_print.layer_id]);
                    gcode += this->extrude_infill(print, by_region_specific, false);
                }
                if (this->config().gcode_label_objects)
                    gcode += std::string("; stop printing object ") + instance_to_print.print_object.model_object()->name 
                        + " id:" + std::to_string((std::find(this->m_ordered_objects.begin(), this->m_ordered_objects.end(), &instance_to_print.print_object) - this->m_ordered_objects.begin())) 
                        + " copy " + std::to_string(instance_to_print.instance_id) + "\n";
            }
        }
    }

    // Apply spiral vase post-processing if this layer contains suitable geometry
    // (we must feed all the G-code into the post-processor, including the first 
    // bottom non-spiral layers otherwise it will mess with positions)
    // we apply spiral vase at this stage because it requires a full layer.
    // Just a reminder: A spiral vase mode is allowed for a single object per layer, single material print only.
    if (m_spiral_vase)
        gcode = m_spiral_vase->process_layer(gcode);


    //add milling post-process if enabled
    if (!config().milling_diameter.values.empty()) {
        bool milling_ok = false;
        for (const LayerToPrint& ltp : layers) {
            if (ltp.object_layer != nullptr) {
                for (const LayerRegion* lr : ltp.object_layer->regions()) {
                    if (!lr->milling.empty()) {
                        milling_ok = true;
                        break;
                    }
                }
            }
        }
        if (milling_ok) {
            //switch to mill
            gcode += "; milling ok\n";
            uint32_t current_extruder_filament = m_writer.tool()->id();
            uint32_t milling_extruder_id = uint32_t(config().nozzle_diameter.values.size());
            gcode += "; toolchange:\n";
            gcode += m_writer.toolchange(milling_extruder_id);
            gcode += "; toolchange done\n";
            m_placeholder_parser.set("current_extruder", milling_extruder_id);
            // Append the filament start G-code.
            const std::string& start_mill_gcode = m_config.milling_toolchange_start_gcode.get_at(0);
            if (!start_mill_gcode.empty()) {
                DynamicConfig config;
                config.set_key_value("previous_extruder", new ConfigOptionInt((int)current_extruder_filament));
                config.set_key_value("next_extruder", new ConfigOptionInt((int)milling_extruder_id));
                config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
                config.set_key_value("previous_layer_z", new ConfigOptionFloat(previous_print_z));
                config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
                // Process the start_mill_gcode for the new filament.
                gcode += this->placeholder_parser_process("milling_toolchange_start_gcode", start_mill_gcode, current_extruder_filament, &config);
                check_add_eol(gcode);
            }

            gcode += "\n; began print:";
            for (const LayerToPrint& ltp : layers) {
                if (ltp.object_layer != nullptr) {
                    for (const PrintInstance& print_instance : ltp.object()->instances()){
                        this->set_origin(unscale(print_instance.shift));
                        for (const LayerRegion* lr : ltp.object_layer->regions()) {
                            if (!lr->milling.empty()) {
                                //EXTRUDE MOVES
                                gcode += "; extrude lr->milling\n";
                                gcode += this->extrude_entity(lr->milling, "; milling post-process");
                            }
                        }
                    }
                }
            }

            //switch to extruder
            m_placeholder_parser.set("current_extruder", milling_extruder_id);
            // Append the filament start G-code.
            const std::string& end_mill_gcode = m_config.milling_toolchange_end_gcode.get_at(0);
            if (!end_mill_gcode.empty()) {
                DynamicConfig config;
                config.set_key_value("previous_extruder", new ConfigOptionInt((int)milling_extruder_id));
                config.set_key_value("next_extruder", new ConfigOptionInt((int)current_extruder_filament));
                config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
                config.set_key_value("previous_layer_z", new ConfigOptionFloat(previous_print_z));
                config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
                // Process the end_mill_gcode for the new filament.
                gcode += this->placeholder_parser_process("milling_toolchange_start_gcode", end_mill_gcode, current_extruder_filament, &config);
                check_add_eol(gcode);
            }
            gcode += "; will go back to normal extruder\n";
            //TODO: change wipetower code to add an other filament change per layer.
            gcode += (layer_tools.has_wipe_tower && m_wipe_tower) ?
                m_wipe_tower->tool_change(*this, current_extruder_filament, current_extruder_filament == layer_tools.extruders.back()) :
                this->set_extruder(current_extruder_filament, print_z);
        }
    }


    // Apply cooling logic; this may alter speeds.
    if (m_cooling_buffer)
        gcode = m_cooling_buffer->process_layer(gcode, layer.id());

    // add tag for analyzer
    if (gcode.find(GCodeAnalyzer::Pause_Print_Tag) != gcode.npos)
        gcode += "\n; " + GCodeAnalyzer::End_Pause_Print_Or_Custom_Code_Tag + "\n";
    else if (gcode.find(GCodeAnalyzer::Custom_Code_Tag) != gcode.npos)
        gcode += "\n; " + GCodeAnalyzer::End_Pause_Print_Or_Custom_Code_Tag + "\n";

#ifdef HAS_PRESSURE_EQUALIZER
    // Apply pressure equalization if enabled;
    // printf("G-code before filter:\n%s\n", gcode.c_str());
    if (m_pressure_equalizer)
        gcode = m_pressure_equalizer->process(gcode.c_str(), false);
    // printf("G-code after filter:\n%s\n", out.c_str());
#endif /* HAS_PRESSURE_EQUALIZER */
    
    _write(file, gcode);
    BOOST_LOG_TRIVIAL(trace) << "Exported layer " << layer.id() << " print_z " << print_z << 
        ", time estimator memory: " <<
            format_memsize_MB(m_normal_time_estimator.memory_used() + (m_silent_time_estimator_enabled ? m_silent_time_estimator.memory_used() : 0)) <<
        ", analyzer memory: " <<
            format_memsize_MB(m_analyzer.memory_used()) <<
        log_memory_info();
}

void GCode::apply_print_config(const PrintConfig &print_config)
{
    m_writer.apply_print_config(print_config);
    m_config.apply(print_config);
}

void GCode::append_full_config(const Print &print, std::string &str)
{
	const DynamicPrintConfig &cfg = print.full_print_config();
    // Sorted list of config keys, which shall not be stored into the G-code. Initializer list.
	const std::vector<std::string> banned_keys { 
		"compatible_printers",
		"compatible_prints",
		"print_host",
		"printhost_apikey",
		"printhost_cafile"
	};
    assert(std::is_sorted(banned_keys.begin(), banned_keys.end()));
	auto is_banned = [banned_keys](const std::string &key) {
		return std::binary_search(banned_keys.begin(), banned_keys.end(), key);
	};
    for (const std::string &key : cfg.keys())
        if (! is_banned(key) && ! cfg.option(key)->is_nil())
            str += "; " + key + " = " + cfg.opt_serialize(key) + "\n";
}

void GCode::set_extruders(const std::vector<uint16_t>& extruder_ids)
{
    m_writer.set_extruders(extruder_ids);

    // enable wipe path generation if any extruder has wipe enabled
    m_wipe.enable = false;
    for (auto id : extruder_ids)
        if (m_config.wipe.get_at(id)) {
            m_wipe.enable = true;
            break;
        }
}

void GCode::set_origin(const Vec2d &pointf)
{    
    // if origin increases (goes towards right), last_pos decreases because it goes towards left
    const Point translate(
        scale_(m_origin(0) - pointf(0)),
        scale_(m_origin(1) - pointf(1))
    );
    m_last_pos += translate;
    m_wipe.path.translate(translate);
    m_origin = pointf;
}

std::string GCode::preamble()
{
    std::string gcode = m_writer.preamble();
    
    /*  Perform a *silent* move to z_offset: we need this to initialize the Z
        position of our writer object so that any initial lift taking place
        before the first layer change will raise the extruder from the correct
        initial Z instead of 0.  */
    m_writer.travel_to_z(m_config.z_offset.value);
    
    return gcode;
}

// called by GCode::process_layer()
std::string GCode::change_layer(coordf_t print_z)
{
    std::string gcode;
    if (m_layer_count > 0)
        // Increment a progress bar indicator.
        gcode += m_writer.update_progress(++ m_layer_index, m_layer_count);
    coordf_t z = print_z + m_config.z_offset.value;  // in unscaled coordinates
    if (BOOL_EXTRUDER_CONFIG(retract_layer_change) && m_writer.will_move_z(z))
        gcode += this->retract();

    {
        std::ostringstream comment;
        comment << "move to next layer (" << m_layer_index << ")";
        gcode += m_writer.travel_to_z(z, comment.str());
    }
    
    // forget last wiping path as wiping after raising Z is pointless
    m_wipe.reset_path();
    
    return gcode;
}

// Return a value in <0, 1> of a cubic B-spline kernel centered around zero.
// The B-spline is re-scaled so it has value 1 at zero.
static inline float bspline_kernel(float x)
{
    x = std::abs(x);
	if (x < 1.f) {
		return 1.f - (3.f / 2.f) * x * x + (3.f / 4.f) * x * x * x;
	}
	else if (x < 2.f) {
		x -= 1.f;
		float x2 = x * x;
		float x3 = x2 * x;
		return (1.f / 4.f) - (3.f / 4.f) * x + (3.f / 4.f) * x2 - (1.f / 4.f) * x3;
	}
	else
        return 0;
}

static float extrudate_overlap_penalty(float nozzle_r, float weight_zero, float overlap_distance)
{
    // The extrudate is not fully supported by the lower layer. Fit a polynomial penalty curve.
    // Solved by sympy package:
/*
from sympy import *
(x,a,b,c,d,r,z)=symbols('x a b c d r z')
p = a + b*x + c*x*x + d*x*x*x
p2 = p.subs(solve([p.subs(x, -r), p.diff(x).subs(x, -r), p.diff(x,x).subs(x, -r), p.subs(x, 0)-z], [a, b, c, d]))
from sympy.plotting import plot
plot(p2.subs(r,0.2).subs(z,1.), (x, -1, 3), adaptive=False, nb_of_points=400)
*/
    if (overlap_distance < - nozzle_r) {
        // The extrudate is fully supported by the lower layer. This is the ideal case, therefore zero penalty.
        return 0.f;
    } else {
        float x  = overlap_distance / nozzle_r;
        float x2 = x * x;
        float x3 = x2 * x;
        return weight_zero * (1.f + 3.f * x + 3.f * x2 + x3);
    }
}

static Points::iterator project_point_to_polygon_and_insert(Polygon &polygon, const Point &pt, double eps)
{
    assert(polygon.points.size() >= 2);
    if (polygon.points.size() <= 1)
    if (polygon.points.size() == 1)
        return polygon.points.begin();

    Point  pt_min;
    double d_min = std::numeric_limits<double>::max();
    size_t i_min = size_t(-1);

    for (size_t i = 0; i < polygon.points.size(); ++ i) {
        size_t j = i + 1;
        if (j == polygon.points.size())
            j = 0;
        const Point &p1 = polygon.points[i];
        const Point &p2 = polygon.points[j];
        const Slic3r::Point v_seg = p2 - p1;
        const Slic3r::Point v_pt  = pt - p1;
        //going into double because coord*coord can overflow.
        const double l2_seg = double(v_seg(0)) * double(v_seg(0)) + double(v_seg(1)) * double(v_seg(1));
        double t_pt = double(v_seg(0)) * double(v_pt(0)) + double(v_seg(1)) * double(v_pt(1));
        if (t_pt < 0) {
            // Closest to p1.
            double dabs = sqrt(double(v_pt(0)) * double(v_pt(0)) + double(v_pt(1)) * double(v_pt(1)));
            if (dabs < d_min) {
                d_min  = dabs;
                i_min  = i;
                pt_min = p1;
            }
        }
        else if (t_pt > l2_seg) {
            // Closest to p2. Then p2 is the starting point of another segment, which shall be discovered in the next step.
            continue;
        } else {
            // Closest to the segment.
            assert(t_pt >= 0 && t_pt <= l2_seg);
            double d_seg = double(v_seg(1)) * double(v_pt(0)) - double(v_seg(0)) * double(v_pt(1));
            double d = double(d_seg) / sqrt(double(l2_seg));
            double dabs = std::abs(d);
            if (dabs < d_min) {
                d_min  = dabs;
                i_min  = i;
                // Evaluate the foot point.
                pt_min = p1;
                double linv = double(d_seg) / double(l2_seg);
                pt_min(0) = pt(0) - coord_t(floor(double(v_seg(1)) * linv + 0.5));
				pt_min(1) = pt(1) + coord_t(floor(double(v_seg(0)) * linv + 0.5));
				assert(Line(p1, p2).distance_to(pt_min) < scale_(1e-5));
            }
        }
    }

	assert(i_min != size_t(-1));
    if ((pt_min - polygon.points[i_min]).cast<double>().norm() > eps) {
        // Insert a new point on the segment i_min, i_min+1.
        return polygon.points.insert(polygon.points.begin() + (i_min + 1), pt_min);
    }
    return polygon.points.begin() + i_min;
}

std::vector<float> polygon_parameter_by_length(const Polygon &polygon)
{
    // Parametrize the polygon by its length.
    std::vector<float> lengths(polygon.points.size()+1, 0.);
    for (size_t i = 1; i < polygon.points.size(); ++ i)
        lengths[i] = lengths[i-1] + (polygon.points[i] - polygon.points[i-1]).cast<float>().norm();
    lengths.back() = lengths[lengths.size()-2] + (polygon.points.front() - polygon.points.back()).cast<float>().norm();
    return lengths;
}

std::vector<float> polygon_angles_at_vertices(const Polygon &polygon, const std::vector<float> &lengths, float min_arm_length)
{
    assert(polygon.points.size() + 1 == lengths.size());
    if (min_arm_length > 0.25f * lengths.back())
        min_arm_length = 0.25f * lengths.back();

    // Find the initial prev / next point span.
    size_t idx_prev = polygon.points.size();
    size_t idx_curr = 0;
    size_t idx_next = 1;
    while (idx_prev > idx_curr && lengths.back() - lengths[idx_prev] < min_arm_length)
        -- idx_prev;
    while (idx_next < idx_prev && lengths[idx_next] < min_arm_length)
        ++ idx_next;

    std::vector<float> angles(polygon.points.size(), 0.f);
    for (; idx_curr < polygon.points.size(); ++ idx_curr) {
        // Move idx_prev up until the distance between idx_prev and idx_curr is lower than min_arm_length.
        if (idx_prev >= idx_curr) {
            while (idx_prev < polygon.points.size() && lengths.back() - lengths[idx_prev] + lengths[idx_curr] > min_arm_length)
                ++ idx_prev;
            if (idx_prev == polygon.points.size())
                idx_prev = 0;
        }
        while (idx_prev < idx_curr && lengths[idx_curr] - lengths[idx_prev] > min_arm_length)
            ++ idx_prev;
        // Move idx_prev one step back.
        if (idx_prev == 0)
            idx_prev = polygon.points.size() - 1;
        else
            -- idx_prev;
        // Move idx_next up until the distance between idx_curr and idx_next is greater than min_arm_length.
        if (idx_curr <= idx_next) {
            while (idx_next < polygon.points.size() && lengths[idx_next] - lengths[idx_curr] < min_arm_length)
                ++ idx_next;
            if (idx_next == polygon.points.size())
                idx_next = 0;
        }
        while (idx_next < idx_curr && lengths.back() - lengths[idx_curr] + lengths[idx_next] < min_arm_length)
            ++ idx_next;
        // Calculate angle between idx_prev, idx_curr, idx_next.
        const Point &p0 = polygon.points[idx_prev];
        const Point &p1 = polygon.points[idx_curr];
        const Point &p2 = polygon.points[idx_next];
        const Point  v1 = p1 - p0;
        const Point  v2 = p2 - p1;
        double dot   = double(v1(0))*double(v2(0)) + double(v1(1))*double(v2(1));
        double cross = double(v1(0))*double(v2(1)) - double(v1(1))*double(v2(0));
        float angle = float(atan2(cross, dot));
        angles[idx_curr] = angle;
    }

    return angles;
}

//like extrude_loop but with varying z and two full round
std::string GCode::extrude_loop_vase(const ExtrusionLoop &original_loop, const std::string &description, double speed, std::unique_ptr<EdgeGrid::Grid> *lower_layer_edge_grid)
{
    //don't keep the speed
    speed = -1;
    // get a copy; don't modify the orientation of the original loop object otherwise
    // next copies (if any) would not detect the correct orientation
    ExtrusionLoop loop = original_loop;

    if (m_layer->lower_layer != nullptr && lower_layer_edge_grid != nullptr) {
        if (!*lower_layer_edge_grid) {
            // Create the distance field for a layer below.
            const coord_t distance_field_resolution = coord_t(scale_(1.) + 0.5);
            *lower_layer_edge_grid = make_unique<EdgeGrid::Grid>();
            (*lower_layer_edge_grid)->create(m_layer->lower_layer->lslices, distance_field_resolution);
            (*lower_layer_edge_grid)->calculate_sdf();
#if 0
            {
                static int iRun = 0;
                BoundingBox bbox = (*lower_layer_edge_grid)->bbox();
                bbox.min(0) -= scale_(5.f);
                bbox.min(1) -= scale_(5.f);
                bbox.max(0) += scale_(5.f);
                bbox.max(1) += scale_(5.f);
                EdgeGrid::save_png(*(*lower_layer_edge_grid), bbox, scale_(0.1f), debug_out_path("GCode_extrude_loop_edge_grid-%d.png", iRun++));
            }
#endif
        }
    }

    // extrude all loops ccw
    //no! this was decided in perimeter_generator
    bool is_hole_loop = (loop.loop_role() & ExtrusionLoopRole::elrHole) != 0;// loop.make_counter_clockwise();
    bool reverse_turn = loop.polygon().is_clockwise() ^ is_hole_loop;

    split_at_seam_pos(loop, lower_layer_edge_grid);

    // clip the path to avoid the extruder to get exactly on the first point of the loop;
    // if polyline was shorter than the clipping distance we'd get a null polyline, so
    // we discard it in that case
    double clip_length = m_enable_loop_clipping ?
        scale_(EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter,0)) * LOOP_CLIPPING_LENGTH_OVER_NOZZLE_DIAMETER :
        0;

    // get paths
    ExtrusionPaths paths;
    loop.clip_end(clip_length, &paths);
    if (paths.empty()) return "";

    // apply the small/external? perimeter speed
    if (is_perimeter(paths.front().role()) && loop.length() <= SMALL_PERIMETER_LENGTH && speed == -1)
        speed = m_config.external_perimeter_speed.get_abs_value(m_config.perimeter_speed);

    //get extrusion length
    coordf_t length = 0;
    for (ExtrusionPaths::iterator path = paths.begin(); path != paths.end(); ++path) {
        //path->simplify(SCALED_RESOLUTION); //not useful, this should have been done before.
        length += path->length() * SCALING_FACTOR;
    }
    
    //all in unscaled coordinates (hence why it's coordf_t and not coord_t)
    const coordf_t min_height = EXTRUDER_CONFIG_WITH_DEFAULT(min_layer_height, this->m_layer->height);
    const coordf_t bot_init_z = - this->m_layer->height;
    //const coordf_t bot_last_z = bot_init_z + this->m_layer->height - EXTRUDER_CONFIG(min_layer_height);
    const coordf_t init_z = bot_init_z + min_height;
    //const coordf_t last_z = bot_init_z + this->m_layer->height;

    Point inward_point;
    //move the seam point inward a little bit
    if (paths.back().role() == erExternalPerimeter && m_layer != NULL && m_config.perimeters.value > 1 && paths.front().size() >= 2 && paths.back().polyline.points.size() >= 3) {
        // detect angle between last and first segment
        // the side depends on the original winding order of the polygon (left for contours, right for holes)
        //FIXME improve the algorithm in case the loop is tiny.
        //FIXME improve the algorithm in case the loop is split into segments with a low number of points (see the Point b query).
        Point a = paths.front().polyline.points[1];  // second point
        Point b = *(paths.back().polyline.points.end() - 3);       // second to last point
        if (reverse_turn) {
            // swap points
            Point c = a; a = b; b = c;
        }

        double angle = paths.front().first_point().ccw_angle(a, b)*2 / 3;

        // turn left if contour, turn right if hole
        if (reverse_turn) angle *= -1;

        // create the destination point along the first segment and rotate it
        // we make sure we don't exceed the segment length because we don't know
        // the rotation of the second segment so we might cross the object boundary
        Vec2d  p1 = paths.front().polyline.points.front().cast<double>();
        Vec2d  p2 = paths.front().polyline.points[1].cast<double>();
        Vec2d  v = p2 - p1;
        double nd = scale_(EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter, paths.front().width));
        double l2 = v.squaredNorm();
        // Shift by no more than a nozzle diameter.
        //FIXME Hiding the seams will not work nicely for very densely discretized contours!
        inward_point = ((nd * nd >= l2) ? p2 : (p1 + v * (nd / sqrt(l2)))).cast<coord_t>();
        inward_point.rotate(angle, paths.front().polyline.points.front());
    }

    coordf_t current_pos_in_length = 0;
    coordf_t current_z = 0; // over init_z
    coordf_t current_height = min_height;
    coordf_t starting_height = min_height;
    enum Step {
        INCR = 0,
        FLAT = 1
    };
    std::string gcode;
    for (int step = 0; step < 2; step++) {
        current_pos_in_length = 0;
        current_z = 0;
        const coordf_t z_per_length = (step == Step::INCR) ? ((this->m_layer->height - (min_height + min_height)) / length) : 0;
        const coordf_t height_per_length = (step == Step::INCR) ? ((this->m_layer->height- (min_height + min_height)) / length) : ((-this->m_layer->height + (min_height + min_height)) / length);
        if (step == Step::FLAT) {
            current_height = this->m_layer->height - min_height;
            starting_height = this->m_layer->height - min_height;
        }
        Vec3d previous;
        for (ExtrusionPaths::iterator path = paths.begin(); path != paths.end(); ++path) {
            if (path == paths.begin() ){
                if (step == Step::INCR) {
                    if (paths.back().role() == erExternalPerimeter && m_layer != NULL && m_config.perimeters.value > 1 && paths.front().size() >= 2 && paths.back().polyline.points.size() >= 3) {
                        paths[0].polyline.points.insert(paths[0].polyline.points.begin(), inward_point);
                    }
                    this->m_writer.travel_to_z(this->m_layer->print_z + init_z);
                } else {
                    //ensure we're at the right height
                    this->m_writer.travel_to_z(this->m_layer->print_z);
                }
            }
            gcode += this->_before_extrude(*path, description, speed);
            if (path == paths.begin() && step == Step::INCR){
                if (paths.back().role() == erExternalPerimeter && m_layer != NULL && m_config.perimeters.value > 1 && paths.front().size() >= 2 && paths.back().polyline.points.size() >= 3) {
                    paths[0].polyline.points.erase(paths[0].polyline.points.begin());
                    gcode += m_writer.extrude_to_xy(this->point_to_gcode(paths[0].polyline.points.front()), 0);
                }
            }

            // calculate extrusion length per distance unit
            double e_per_mm_per_height = (path->mm3_per_mm / this->m_layer->height)
                * m_writer.tool()->e_per_mm3()
                * this->config().print_extrusion_multiplier.get_abs_value(1);
            if (m_writer.extrusion_axis().empty()) e_per_mm_per_height = 0;
            {
                std::string comment = m_config.gcode_comments ? description : "";
                for (const Line &line : path->polyline.lines()) {
                    const coordf_t line_length = line.length() * SCALING_FACTOR;
                    //don't go (much) more than a nozzle_size without a refresh of the z & extrusion rate
                    const int nb_sections = std::max(1,int(line_length / EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter, paths.front().width)));
                    const coordf_t height_increment = height_per_length * line_length / nb_sections;
                    Vec3d last_point{ this->point_to_gcode(line.a).x(), this->point_to_gcode(line.a).y(), current_z };
                    const Vec3d pos_increment{ (this->point_to_gcode(line.b).x() - last_point.x()) / nb_sections,
                        (this->point_to_gcode(line.b).y() - last_point.y()) / nb_sections,
                        z_per_length * line_length / nb_sections };
                    coordf_t current_height_internal = current_height + height_increment / 2;
                    //ensure you go to the good xyz
                    if( (last_point - previous).norm() > EPSILON)
                        gcode += m_writer.extrude_to_xyz(last_point, 0, description);
                    //extrusions
                    for (int i = 0; i < nb_sections - 1; i++) {
                        Vec3d new_point = last_point + pos_increment;
                        gcode += m_writer.extrude_to_xyz(new_point,
                            e_per_mm_per_height * (line_length / nb_sections) * current_height_internal,
                            description);
                        current_height_internal += height_increment;
                        last_point = new_point;
                    }
                    //last bit will go to the exact last pos
                    last_point.x() = this->point_to_gcode(line.b).x();
                    last_point.y() = this->point_to_gcode(line.b).y();
                    last_point.z() = current_z + z_per_length * line_length;
                    gcode += m_writer.extrude_to_xyz(
                        last_point,
                        e_per_mm_per_height * (line_length / nb_sections) * current_height_internal,
                        comment);
                    previous = last_point;

                    //update vars for next line
                    current_pos_in_length += line_length;
                    current_z = current_pos_in_length * z_per_length;//last_point.z();
                    current_height = starting_height + current_pos_in_length * height_per_length;
                }
            }
            gcode += this->_after_extrude(*path);
        }
    }

    // reset acceleration
    m_writer.set_acceleration((unsigned int)(m_config.default_acceleration.value + 0.5));

    //don't wipe here
    //if (m_wipe.enable)
    //    m_wipe.path = paths.front().polyline;  // TODO: don't limit wipe to last path

    //just continue on the perimeter a bit while retracting
    //FIXME this doesn't work work, hence why it's commented
    //coordf_t travel_length = std::min(length, EXTRUDER_CONFIG(nozzle_diameter) * 10);
    //for (auto & path : paths){
    //    for (const Line &line : path.polyline.lines()) {
    //        if (unscaled(line.length()) > travel_length) {
    //            // generate the travel move
    //            gcode += m_writer.travel_to_xy(this->point_to_gcode(line.b), "move inwards before travel");
    //            travel_length -= unscaled(line.length());
    //        }
    //        else
    //        {
    //            gcode += m_writer.travel_to_xy(this->point_to_gcode(line.a) + (this->point_to_gcode(line.b) - this->point_to_gcode(line.a)) * (travel_length / unscaled(line.length())), "move before travel");
    //            travel_length = 0;
    //            //double break;
    //            goto FINISH_MOVE;
    //        }
    //    }
    //}
    //FINISH_MOVE:

    // make a little move inwards before leaving loop
    if (paths.back().role() == erExternalPerimeter && m_layer != NULL && m_config.perimeters.value > 1 && paths.front().size() >= 2 && paths.back().polyline.points.size() >= 3) {
        // detect angle between last and first segment
        // the side depends on the original winding order of the polygon (left for contours, right for holes)
        //FIXME improve the algorithm in case the loop is tiny.
        //FIXME improve the algorithm in case the loop is split into segments with a low number of points (see the Point b query).
        Point a = paths.front().polyline.points[1];  // second point
        Point b = *(paths.back().polyline.points.end() - 3);       // second to last point
        if (reverse_turn) {
            // swap points
            Point c = a; a = b; b = c;
        }

        double angle = paths.front().first_point().ccw_angle(a, b) / 3;

        // turn left if contour, turn right if hole
        if (reverse_turn) angle *= -1;

        // create the destination point along the first segment and rotate it
        // we make sure we don't exceed the segment length because we don't know
        // the rotation of the second segment so we might cross the object boundary
        Vec2d  p1 = paths.front().polyline.points.front().cast<double>();
        Vec2d  p2 = paths.front().polyline.points[1].cast<double>();
        Vec2d  v = p2 - p1;
        double nd = scale_(EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter, paths.front().width));
        double l2 = v.squaredNorm();
        // Shift by no more than a nozzle diameter.
        //FIXME Hiding the seams will not work nicely for very densely discretized contours!
        inward_point = ((nd * nd >= l2) ? p2 : (p1 + v * (nd / sqrt(l2)))).cast<coord_t>();
        inward_point.rotate(angle, paths.front().polyline.points.front());
        
        // generate the travel move
        gcode += m_writer.travel_to_xy(this->point_to_gcode(inward_point), "move inwards before travel");
    }

    return gcode;
}

void GCode::split_at_seam_pos(ExtrusionLoop &loop, std::unique_ptr<EdgeGrid::Grid> *lower_layer_edge_grid)
{
    if (loop.paths.empty())
        return;

    SeamPosition seam_position = m_config.seam_position;
    if (loop.loop_role() == elrSkirt)
        seam_position = spNearest;

    // find the point of the loop that is closest to the current extruder position
    // or randomize if requested
    Point last_pos = this->last_pos();
    if (m_config.spiral_vase) {
        loop.split_at(last_pos, false);
    } else if (seam_position == spNearest || seam_position == spAligned || seam_position == spRear || seam_position == spHidden || seam_position == spCustom) {
        Polygon        polygon = loop.polygon();
        const coordf_t nozzle_dmr = EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter, loop.paths.front().width);
        const coord_t  nozzle_r = coord_t(scale_(0.5 * nozzle_dmr) + 0.5);

        // Retrieve the last start position for this object.
        float last_pos_weight = 1.f;

        bool has_seam_custom = false;
        for (ModelVolume *v : m_layer->object()->model_object()->volumes)
            if (v->is_seam_position()) {
                has_seam_custom = true;
                break;
            }
        if (has_seam_custom) {
            // Seam is aligned to be nearest to the position of a "lambda-seam"-named modifier in this or any preceding layer
            last_pos = m_layer->object()->bounding_box().center();
            // Look for all lambda-seam-modifiers below current z, choose the highest one
            ModelVolume *v_lambda_seam = nullptr;
            Vec3d lambda_pos;
            double lambda_dist;
            double lambda_radius;
            for (ModelVolume *v : m_layer->object()->model_object()->volumes)
                if (v->is_seam_position()) {
                    //xy in object coordinates, z in plater coordinates
                    Vec3d test_lambda_pos = m_layer->object()->model_object()->instances.front()->transform_vector(v->get_offset(), true);
                    Vec3d test_lambda_pos_plater = m_layer->object()->model_object()->instances.front()->transform_vector(v->get_offset(), false);
                    Point xy_lambda(scale_(test_lambda_pos.x()), scale_(test_lambda_pos.y()));
                    Point nearest = polygon.point_projection(xy_lambda);
                    Vec3d polygon_3dpoint{ unscaled(nearest.x()), unscaled(nearest.y()), (double)m_layer->print_z };
                    double test_lambda_dist = (polygon_3dpoint - test_lambda_pos).norm();
                    double sphere_radius = m_layer->object()->model_object()->instances.front()->transform_bounding_box(v->mesh().bounding_box(), true).size().x() / 2;
                    //if (test_lambda_dist > sphere_radius)
                    //    continue;

                    //use this one if the first or nearer (in z)
                    if (v_lambda_seam == nullptr || lambda_dist > test_lambda_dist) {
                        v_lambda_seam = v;
                        lambda_pos = test_lambda_pos;
                        lambda_radius = sphere_radius;
                        lambda_dist = test_lambda_dist;
                    }
                }

            if (v_lambda_seam != nullptr) {
                lambda_pos = m_layer->object()->model_object()->instances.front()->transform_vector(v_lambda_seam->get_offset(), true);
                // Found, get the center point and apply rotation and scaling of Model instance. Continues to spAligned if not found or Weight set to Zero.
                last_pos = Point::new_scale(lambda_pos.x(), lambda_pos.y());
                // Weight is set by user and stored in the radius of the sphere
                last_pos_weight = std::max(0.0, std::round(100 * (lambda_radius)));
                if (last_pos_weight > 0.0)
                    seam_position = spCustom;
            }
        }
        if (seam_position == spAligned) {
            // Seam is aligned to the seam at the preceding layer.
            if (m_layer != NULL && m_seam_position.count(m_layer->object()) > 0) {
                last_pos = m_seam_position[m_layer->object()];
                last_pos_weight = 1.f;
            }
        } else if (seam_position == spNearest) {
            last_pos_weight = 5.f;
        } else if (seam_position == spRear) {
            // Object is centered around (0,0) in its current coordinate system.
            last_pos.x() = 0;
            last_pos.y() += coord_t(3. * m_layer->object()->bounding_box().radius());
            last_pos_weight = 5.f;
        } else if (seam_position == spHidden) {
            last_pos_weight = 1.f;
        }

        // Insert a projection of last_pos into the polygon.
        size_t last_pos_proj_idx;
        {
            Points::iterator it = project_point_to_polygon_and_insert(polygon, last_pos, 0.1 * nozzle_r);
            last_pos_proj_idx = it - polygon.points.begin();
        }
        Point last_pos_proj = polygon.points[last_pos_proj_idx];

        // Parametrize the polygon by its length.
        std::vector<float> lengths = polygon_parameter_by_length(polygon);

        // For each polygon point, store a penalty.
        // First calculate the angles, store them as penalties. The angles are caluculated over a minimum arm length of nozzle_r.
        std::vector<float> penalties = polygon_angles_at_vertices(polygon, lengths, float(nozzle_r));
        // No penalty for reflex points, slight penalty for convex points, high penalty for flat surfaces.
        const float penaltyConvexVertex = 1.f;
        const float penaltyFlatSurface = 5.f;
        const float penaltyOverhangHalf = 10.f;
        // Penalty for visible seams.
        float dist_max = 0.1f * lengths.back();// 5.f * nozzle_dmr
        if (this->config().seam_travel) {
            dist_max = 0;
            for (size_t i = 0; i < polygon.points.size(); ++i) {
                dist_max = std::max(dist_max, (float)polygon.points[i].distance_to(last_pos_proj));
            }
        }
        //TODO: ignore the angle penalty if the new point is not in an external path (bot/top/ext_peri)
        for (size_t i = 0; i < polygon.points.size(); ++i) {
            float ccwAngle = penalties[i];
            //if (was_cw_and_now_ccw) //FIXME
                //ccwAngle = -ccwAngle;
            float penalty = 0;
            //            if (ccwAngle <- float(PI/3.))
            if (ccwAngle <-float(0.6 * PI))
                // Sharp reflex vertex. We love that, it hides the seam perfectly.
                penalty = 0.f;
            //            else if (ccwAngle > float(PI/3.))
            else if (ccwAngle > float(0.6 * PI))
                // Seams on sharp convex vertices are more visible than on reflex vertices.
                penalty = penaltyConvexVertex;
            else if (ccwAngle < 0.f) {
                // Interpolate penalty between maximum and zero.
                penalty = penaltyFlatSurface * bspline_kernel(ccwAngle * float(PI * 2. / 3.));
            } else {
                assert(ccwAngle >= 0.f);
                // Interpolate penalty between maximum and the penalty for a convex vertex.
                penalty = penaltyConvexVertex + (penaltyFlatSurface - penaltyConvexVertex) * bspline_kernel(ccwAngle * float(PI * 2. / 3.));
            }
            if (this->config().seam_travel) {
                penalty += last_pos_weight * polygon.points[i].distance_to(last_pos_proj) / dist_max;
            } else {
                // Give a negative penalty for points close to the last point or the prefered seam location.
                float dist_to_last_pos_proj = (i < last_pos_proj_idx) ?
                    std::min(lengths[last_pos_proj_idx] - lengths[i], lengths.back() - lengths[last_pos_proj_idx] + lengths[i]) :
                    std::min(lengths[i] - lengths[last_pos_proj_idx], lengths.back() - lengths[i] + lengths[last_pos_proj_idx]);
                penalty -= last_pos_weight * bspline_kernel(dist_to_last_pos_proj / dist_max);
            }
            // Allow negative penalties to overcome penalty for overhangs when in Custom mode
            penalties[i] = (seam_position == spCustom) ? penalty : std::max(0.f, penalty);

        }

        // Penalty for overhangs.
        if (lower_layer_edge_grid && (*lower_layer_edge_grid)) {
            // Use the edge grid distance field structure over the lower layer to calculate overhangs.
            coord_t nozzle_r = coord_t(floor(scale_(0.5 * nozzle_dmr) + 0.5));
            coord_t search_r = coord_t(floor(scale_(0.8 * nozzle_dmr) + 0.5));
            for (size_t i = 0; i < polygon.points.size(); ++i) {
                const Point &p = polygon.points[i];
                coordf_t dist;
                // Signed distance is positive outside the object, negative inside the object.
                // The point is considered at an overhang, if it is more than nozzle radius
                // outside of the lower layer contour.
#ifdef NDEBUG // to suppress unused variable warning in release mode
                (*lower_layer_edge_grid)->signed_distance(p, search_r, dist);
#else
                bool found = (*lower_layer_edge_grid)->signed_distance(p, search_r, dist);
#endif
                // If the approximate Signed Distance Field was initialized over lower_layer_edge_grid,
                // then the signed distnace shall always be known.
                assert(found);
                penalties[i] += extrudate_overlap_penalty(float(nozzle_r), penaltyOverhangHalf, float(dist));
            }
        }

        // Find a point with a minimum penalty.
        size_t idx_min = std::min_element(penalties.begin(), penalties.end()) - penalties.begin();

        // if (seam_position == spAligned)
        // For all (aligned, nearest, rear, hidden, custom) seams:
        {
            // Very likely the weight of idx_min is very close to the weight of last_pos_proj_idx.
            // In that case use last_pos_proj_idx instead.
            float penalty_aligned = penalties[last_pos_proj_idx];
            float penalty_min = penalties[idx_min];
            float penalty_diff_abs = std::abs(penalty_min - penalty_aligned);
            float penalty_max = std::max(penalty_min, penalty_aligned);
            float penalty_diff_rel = (penalty_max == 0.f) ? 0.f : penalty_diff_abs / penalty_max;
            // printf("Align seams, penalty aligned: %f, min: %f, diff abs: %f, diff rel: %f\n", penalty_aligned, penalty_min, penalty_diff_abs, penalty_diff_rel);
            if (penalty_diff_rel < 0.05) {
                // Penalty of the aligned point is very close to the minimum penalty.
                // Align the seams as accurately as possible.
                idx_min = last_pos_proj_idx;
            }
            m_seam_position[m_layer->object()] = polygon.points[idx_min];
        }

        // Export the contour into a SVG file.
        #if 0
        {
            static int iRun = 0;
            SVG svg(debug_out_path("GCode_extrude_loop-%d.svg", iRun ++));
            if (m_layer->lower_layer != NULL)
                svg.draw(m_layer->lower_layer->slices);
            for (size_t i = 0; i < loop.paths.size(); ++ i)
                svg.draw(loop.paths[i].as_polyline(), "red");
            Polylines polylines;
            for (size_t i = 0; i < loop.paths.size(); ++ i)
                polylines.push_back(loop.paths[i].as_polyline());
            Slic3r::Polygons polygons;
            coordf_t nozzle_dmr = EXTRUDER_CONFIG(nozzle_diameter);
            coord_t delta = scale_(0.5*nozzle_dmr);
            Slic3r::offset(polylines, &polygons, delta);
//            for (size_t i = 0; i < polygons.size(); ++ i) svg.draw((Polyline)polygons[i], "blue");
            svg.draw(last_pos, "green", 3);
            svg.draw(polygon.points[idx_min], "yellow", 3);
            svg.Close();
        }
        #endif

        // Split the loop at the point with a minium penalty.
        if (!loop.split_at_vertex(polygon.points[idx_min]))
            // The point is not in the original loop. Insert it.
            loop.split_at(polygon.points[idx_min], true);

    } else if (seam_position == spRandom) {
        if ( (loop.loop_role() & elrInternal) == 0) {
            // This loop does not contain any other (not-hole) loop. Set a random position.
            // The other loops will get a seam close to the random point chosen
            // on the inner most contour.
            //FIXME This works correctly for inner contours first only.
            //FIXME Better parametrize the loop by its length.
            Polygon polygon = loop.polygon();
            Point centroid = polygon.centroid();
            last_pos = Point(polygon.bounding_box().max(0), centroid(1));
            last_pos.rotate(fmod((float)rand() / 16.0, 2.0*PI), centroid);
        }
        // Find the closest point, avoid overhangs.
        loop.split_at(last_pos, true);
    } else {
        loop.split_at(last_pos, false);
    }
}

std::string GCode::extrude_loop(const ExtrusionLoop &original_loop, const std::string &description, double speed, std::unique_ptr<EdgeGrid::Grid> *lower_layer_edge_grid)
{
#if DEBUG_EXTRUSION_OUTPUT
    std::cout << "extrude loop_" << (original_loop.polygon().is_counter_clockwise() ? "ccw" : "clw") << ": ";
    for (const ExtrusionPath &path : original_loop.paths) {
        std::cout << ", path{ ";
        for (const Point &pt : path.polyline.points) {
            std::cout << ", " << floor(100 * unscale<double>(pt.x())) / 100.0 << ":" << floor(100 * unscale<double>(pt.y())) / 100.0;
        }
        std::cout << "}";
    }
    std::cout << "\n";
#endif

    //no-seam code path redirect
    if (original_loop.role() == ExtrusionRole::erExternalPerimeter && (original_loop.loop_role() & elrVase) != 0 && !this->m_config.spiral_vase
        //but not for the first layer
        && this->m_layer->id() > 0
        //exclude if min_layer_height * 2 > layer_height (increase from 2 to 3 because it's working but uses in-between)
        && this->m_layer->height >= EXTRUDER_CONFIG_WITH_DEFAULT(min_layer_height, 0) * 2 - EPSILON
        ) {
        return extrude_loop_vase(original_loop, description, speed, lower_layer_edge_grid);
    }

    // get a copy; don't modify the orientation of the original loop object otherwise
    // next copies (if any) would not detect the correct orientation
    ExtrusionLoop loop = original_loop;

    if (m_layer->lower_layer != nullptr && lower_layer_edge_grid != nullptr) {
        if (! *lower_layer_edge_grid) {
            // Create the distance field for a layer below.
            const coord_t distance_field_resolution = coord_t(scale_(1.) + 0.5);
            *lower_layer_edge_grid = make_unique<EdgeGrid::Grid>();
            (*lower_layer_edge_grid)->create(m_layer->lower_layer->lslices, distance_field_resolution);
            (*lower_layer_edge_grid)->calculate_sdf();
            #if 0
            {
                static int iRun = 0;
                BoundingBox bbox = (*lower_layer_edge_grid)->bbox();
                bbox.min(0) -= scale_(5.f);
                bbox.min(1) -= scale_(5.f);
                bbox.max(0) += scale_(5.f);
                bbox.max(1) += scale_(5.f);
                EdgeGrid::save_png(*(*lower_layer_edge_grid), bbox, scale_(0.1f), debug_out_path("GCode_extrude_loop_edge_grid-%d.png", iRun++));
            }
            #endif
        }
    }
    
    // extrude all loops ccw
    //no! this was decided in perimeter_generator
    //but we need to know where is "inside", so we will use is_hole_loop. if is_hole_loop, then we need toconsider that the right direction is clockwise, else counter clockwise. 
    bool is_hole_loop = (loop.loop_role() & ExtrusionLoopRole::elrHole) != 0;// loop.make_counter_clockwise();

    //if spiral vase, we have to ensure that all loops are in the same orientation.
    if (this->m_config.spiral_vase) {
        loop.make_counter_clockwise();
        is_hole_loop = false;
    }

    split_at_seam_pos(loop, lower_layer_edge_grid);
    
    // clip the path to avoid the extruder to get exactly on the first point of the loop;
    // if polyline was shorter than the clipping distance we'd get a null polyline, so
    // we discard it in that case
    double clip_length = m_enable_loop_clipping ? 
        scale_(EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter,0)) * LOOP_CLIPPING_LENGTH_OVER_NOZZLE_DIAMETER :
        0;

    // get paths
    ExtrusionPaths paths;
    loop.clip_end(clip_length, &paths);
    if (paths.empty()) return "";
    
    // apply the small perimeter speed
    if (is_perimeter(paths.front().role()) && loop.length() <= SMALL_PERIMETER_LENGTH && speed == -1)
        speed = m_config.small_perimeter_speed.get_abs_value(m_config.perimeter_speed);
    
    // extrude along the path
    std::string gcode;
    for (ExtrusionPaths::iterator path = paths.begin(); path != paths.end(); ++path) {
//    description += ExtrusionLoop::role_to_string(loop.loop_role());
//    description += ExtrusionEntity::role_to_string(path->role);
        path->simplify(SCALED_RESOLUTION);
        gcode += this->_extrude(*path, description, speed);
    }
    
    // reset acceleration
    m_writer.set_acceleration((unsigned int)(m_config.default_acceleration.value + 0.5));
    
    if (m_wipe.enable)
        m_wipe.path = paths.front().polyline;  // TODO: don't limit wipe to last path

    //wipe for External Perimeter
    if (paths.back().role() == erExternalPerimeter && m_layer != NULL && m_config.perimeters.value > 1 && paths.front().size() >= 2 && paths.back().polyline.points.size() >= 2) {
        //get points for wipe
        Point prev_point = *(paths.back().polyline.points.end() - 2);       // second to last point
        // *(paths.back().polyline.points.end() - 2) this is the same as (or should be) as paths.front().first_point();
        Point current_point = paths.front().first_point();
        Point next_point = paths.front().polyline.points[1];  // second point

        //extra wipe before the little move.
        if (EXTRUDER_CONFIG_WITH_DEFAULT(wipe_extra_perimeter, 0) > 0) {
            coord_t wipe_dist = scale_(EXTRUDER_CONFIG_WITH_DEFAULT(wipe_extra_perimeter,0));
            ExtrusionPaths paths_wipe;
            for (int i = 0; i < paths.size(); i++) {
                ExtrusionPath& path = paths[i];
                if (path.length() < wipe_dist) {
                    wipe_dist -= path.length();
                    paths_wipe.push_back(path);
                } else {
                    paths_wipe.push_back(path);
                    paths_wipe.back().clip_end(path.length() - wipe_dist);

                    ExtrusionPath next_point_path = path;
                    next_point_path.reverse();
                    next_point_path.clip_end(wipe_dist);
                    next_point_path.reverse();
                    if (next_point_path.size() > 1) {
                        next_point = next_point_path.polyline.points[1];
                    } else if (i + 1 < paths.size()) {
                        next_point = paths[i + 1].first_point();
                    } else {
                        next_point = paths[0].first_point();
                    }
                    break;
                }
            }
            //move
            for (ExtrusionPath& path : paths_wipe) {
                for (Point& pt : path.polyline.points) {
                    prev_point = current_point;
                    current_point = pt;
                    gcode += m_writer.travel_to_xy(this->point_to_gcode(pt), config().gcode_comments ? "; extra wipe" : "");
                }
            }
        }

        // make a little move inwards before leaving loop

        // detect angle between last and first segment
        // the side depends on the original winding order of the polygon (left for contours, right for holes)
        //FIXME improve the algorithm in case the loop is tiny.
        //FIXME improve the algorithm in case the loop is split into segments with a low number of points (see the Point b query).
        Point a = next_point;  // second point
        Point b = prev_point;  // second to last point
        if (is_hole_loop ? loop.polygon().is_counter_clockwise() : loop.polygon().is_clockwise()) {
            // swap points
            Point c = a; a = b; b = c;
        }
        double angle = current_point.ccw_angle(a, b) / 3;
        
        // turn left if contour, turn right if hole
        if (is_hole_loop ? loop.polygon().is_counter_clockwise() : loop.polygon().is_clockwise()) angle *= -1;
        
        // create the destination point along the first segment and rotate it
        // we make sure we don't exceed the segment length because we don't know
        // the rotation of the second segment so we might cross the object boundary
        Vec2d  current_pos = current_point.cast<double>();
        Vec2d  next_pos = next_point.cast<double>();
        Vec2d  vec_dist  = next_pos - current_pos;
        double nd = scale_(EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter,0));
        double l2 = vec_dist.squaredNorm();
        // Shift by no more than a nozzle diameter.
        //FIXME Hiding the seams will not work nicely for very densely discretized contours!
        Point  pt = ((nd * nd >= l2) ? next_pos : (current_pos + vec_dist * (nd / sqrt(l2)))).cast<coord_t>();
        pt.rotate(angle, current_point);
        // generate the travel move
        gcode += m_writer.travel_to_xy(this->point_to_gcode(pt), "move inwards before travel");
    }
    
    return gcode;
}

std::string GCode::extrude_multi_path(const ExtrusionMultiPath &multipath, const std::string &description, double speed) {
    // extrude along the path
    std::string gcode;
    for (ExtrusionPath path : multipath.paths) {
//    description += ExtrusionLoop::role_to_string(loop.loop_role());
//    description += ExtrusionEntity::role_to_string(path->role);
        path.simplify(SCALED_RESOLUTION);
        gcode += this->_extrude(path, description, speed);
    }
    if (m_wipe.enable) {
        m_wipe.path = std::move(multipath.paths.back().polyline);  // TODO: don't limit wipe to last path
        m_wipe.path.reverse();
    }
    // reset acceleration
    m_writer.set_acceleration((unsigned int)floor(m_config.default_acceleration.value + 0.5));
    return gcode;
}

std::string GCode::extrude_multi_path3D(const ExtrusionMultiPath3D &multipath3D, const std::string &description, double speed) {
    // extrude along the path
    std::string gcode;
    for (const ExtrusionPath3D &path : multipath3D.paths) {

        gcode += this->_before_extrude(path, description, speed);

        // calculate extrusion length per distance unit
        double e_per_mm = path.mm3_per_mm
            * m_writer.tool()->e_per_mm3()
            * this->config().print_extrusion_multiplier.get_abs_value(1);
        if (m_writer.extrusion_axis().empty()) e_per_mm = 0;
        double path_length = 0.;
        {
            std::string comment = m_config.gcode_comments ? description : "";
            //for (const Line &line : path.polyline.lines()) {
            for (size_t i = 0; i < path.polyline.points.size() - 1; i++) {
                Line line(path.polyline.points[i], path.polyline.points[i + 1]);
                const double line_length = line.length() * SCALING_FACTOR;
                path_length += line_length;
                gcode += m_writer.extrude_to_xyz(
                    this->point_to_gcode(line.b, path.z_offsets.size()>i+1 ? path.z_offsets[i+1] : 0),
                    e_per_mm * line_length,
                    comment);
            }
        }
        gcode += this->_after_extrude(path);
    }
    if (m_wipe.enable) {
        m_wipe.path = std::move(multipath3D.paths.back().polyline);  // TODO: don't limit wipe to last path
        m_wipe.path.reverse();
    }
    // reset acceleration
    m_writer.set_acceleration((unsigned int)floor(m_config.default_acceleration.value + 0.5));
    return gcode;
}

std::string GCode::extrude_entity(const ExtrusionEntity &entity, const std::string &description, double speed, std::unique_ptr<EdgeGrid::Grid> *lower_layer_edge_grid)
{
    this->visitor_gcode.clear();
    this->visitor_comment = description;
    this->visitor_speed = speed;
    this->visitor_lower_layer_edge_grid = lower_layer_edge_grid;
    entity.visit(*this);
    return this->visitor_gcode;
}

void GCode::use(const ExtrusionEntityCollection &collection) {
    if (collection.no_sort || collection.role() == erMixed) {
        for (const ExtrusionEntity* next_entity : collection.entities) {
            next_entity->visit(*this);
        }
    } else {
        ExtrusionEntityCollection chained = collection.chained_path_from(m_last_pos);
        for (const ExtrusionEntity* next_entity : chained.entities) {
            next_entity->visit(*this);
        }
    }
}

std::string extrusion_role_2_string(const ExtrusionRole &er) {
    switch (er) {
        case erNone: return " none";
        case erPerimeter: return " perimeter";
        case erExternalPerimeter: return " perimeter external";
        case erOverhangPerimeter: return " perimeter overhang";
        case erInternalInfill: return " infill internal";
        case erSolidInfill: return " infill solid";
        case erTopSolidInfill: return " infill solid top";
        case erBridgeInfill: return " infill bridge";
        case erThinWall: return " thin_wall";
        case erGapFill: return " gap_fill";
        case erSkirt: return " skirt";
        case erSupportMaterial: return " support_material";
        case erSupportMaterialInterface: return " support_material_interface";
        case erWipeTower: return " wipe_tower";
        case erMilling: return " milling";
        case erCustom: return " custom";
        case erMixed: return " mixed";
        case erCount: return " count";
    }
    return " unkown";
}

std::string GCode::extrude_path(const ExtrusionPath &path, const std::string &description, double speed) {

    ExtrusionPath simplifed_path = path;
    if (this->config().min_length.value != 0 && !m_last_too_small.empty()) {
        //descr += " trys fusion " + std::to_string(unscaled(m_last_too_small.last_point().x())) + " , " + std::to_string(unscaled(path.first_point().x()));
        //ensure that it's a continous thing
        if (m_last_too_small.last_point().distance_to_square(path.first_point()) < scale_(this->config().min_length)) {
            //descr += " ! fusion " + std::to_string(simplifed_path.polyline.points.size());
            simplifed_path.height = (m_last_too_small.height * m_last_too_small.length() + path.height * path.length()) / (m_last_too_small.length() + path.length());
            simplifed_path.mm3_per_mm = (m_last_too_small.mm3_per_mm * m_last_too_small.length() + path.mm3_per_mm * path.length()) / (m_last_too_small.length() + path.length());
            simplifed_path.polyline.points.insert(simplifed_path.polyline.points.begin(), m_last_too_small.polyline.points.begin(), m_last_too_small.polyline.points.end()-1);
        }
        m_last_too_small.polyline.points.clear();
    }
    simplifed_path.simplify(this->config().min_length.value != 0 ? scale_(this->config().min_length) : SCALED_RESOLUTION);
    if (this->config().min_length.value != 0 && simplifed_path.length() < scale_(this->config().min_length)) {
        m_last_too_small = simplifed_path;
        return "";
            //"; "+ descr+" .... too small for extrude: "+std::to_string(simplifed_path.length())+" < "+ std::to_string(scale_(this->config().min_length))
            //+ " ; " + std::to_string(unscaled(path.first_point().x())) + " : " + std::to_string(unscaled(path.last_point().x()))
            //+" =;=> " + std::to_string(unscaled(simplifed_path.first_point().x())) + " : " + std::to_string(unscaled(simplifed_path.last_point().x()))
            //+ "\n";
    }

    std::string gcode = this->_extrude(simplifed_path, description, speed);

    //gcode += " ; " + std::to_string(unscaled(path.first_point().x())) + " : " + std::to_string(unscaled(path.last_point().x()));
    //gcode += " =;=> " + std::to_string(unscaled(simplifed_path.first_point().x())) + " : " + std::to_string(unscaled(simplifed_path.last_point().x()));

    if (m_wipe.enable) {
        m_wipe.path = std::move(simplifed_path.polyline);
        m_wipe.path.reverse();
    }
    // reset acceleration
    m_writer.set_acceleration((unsigned int)floor(m_config.default_acceleration.value + 0.5));
    return gcode;
}

std::string GCode::extrude_path_3D(const ExtrusionPath3D &path, const std::string &description, double speed) {
    //path.simplify(SCALED_RESOLUTION);
    std::string gcode = this->_before_extrude(path, description, speed);

    // calculate extrusion length per distance unit
    double e_per_mm = path.mm3_per_mm
        * m_writer.tool()->e_per_mm3()
        * this->config().print_extrusion_multiplier.get_abs_value(1);
    if (m_writer.extrusion_axis().empty()) e_per_mm = 0;
    double path_length = 0.;
    {
        std::string comment = m_config.gcode_comments ? description : "";
        //for (const Line &line : path.polyline.lines()) {
        for (size_t i = 0; i < path.polyline.points.size()-1;i++) {
            Line line(path.polyline.points[i], path.polyline.points[i + 1]);
            const double line_length = line.length() * SCALING_FACTOR;
            path_length += line_length;
            gcode += m_writer.extrude_to_xyz(
                this->point_to_gcode(line.b, path.z_offsets.size()>i ? path.z_offsets[i] : 0),
                e_per_mm * line_length,
                comment);
        }
    }
    gcode += this->_after_extrude(path);

    if (m_wipe.enable) {
        m_wipe.path = std::move(path.polyline);
        m_wipe.path.reverse();
    }
    // reset acceleration
    m_writer.set_acceleration((unsigned int)floor(m_config.default_acceleration.value + 0.5));
    return gcode;
}

// Extrude perimeters: Decide where to put seams (hide or align seams).
std::string GCode::extrude_perimeters(const Print &print, const std::vector<ObjectByExtruder::Island::Region> &by_region, std::unique_ptr<EdgeGrid::Grid> &lower_layer_edge_grid)
{
    std::string gcode;
    for (const ObjectByExtruder::Island::Region &region : by_region)
        if (! region.perimeters.empty()) {
            m_config.apply(print.regions()[&region - &by_region.front()]->config());
            for (const ExtrusionEntity *ee : region.perimeters)
                gcode += this->extrude_entity(*ee, "", -1., &lower_layer_edge_grid);
        }
    return gcode;
}

// Chain the paths hierarchically by a greedy algorithm to minimize a travel distance.
std::string GCode::extrude_infill(const Print &print, const std::vector<ObjectByExtruder::Island::Region> &by_region, bool is_infill_first)
{
    std::string gcode;
    for (const ObjectByExtruder::Island::Region &region : by_region) {
        if (print.regions()[&region - &by_region.front()]->config().infill_first == is_infill_first) {
            m_config.apply(print.regions()[&region - &by_region.front()]->config());
            ExtrusionEntitiesPtr extrusions { region.infills };
            chain_and_reorder_extrusion_entities(extrusions, &m_last_pos);
            for (const ExtrusionEntity *fill : extrusions) {
                gcode += extrude_entity(*fill, "");
            }
        }
    }
    return gcode;
}

std::string GCode::extrude_support(const ExtrusionEntityCollection &support_fills)
{
    std::string gcode;
    if (! support_fills.entities.empty()) {
        const char   *support_label            = "support material";
        const char   *support_interface_label  = "support material interface";
        const double  support_speed            = m_config.support_material_speed.value;
        const double  support_interface_speed  = m_config.support_material_interface_speed.get_abs_value(support_speed);
        for (const ExtrusionEntity *ee : support_fills.entities) {
            ExtrusionRole role = ee->role();
            assert(role == erSupportMaterial || role == erSupportMaterialInterface || role == erMixed);
            if (const ExtrusionEntityCollection* coll = dynamic_cast<const ExtrusionEntityCollection*>(ee)) {
                gcode += extrude_support(*coll);
                continue;
            }
            const char  *label = (role == erSupportMaterial) ? support_label : support_interface_label;
            const double speed = (role == erSupportMaterial) ? support_speed : support_interface_speed;
            visitor_gcode = "";
            visitor_comment = label;
            visitor_speed = speed;
            visitor_lower_layer_edge_grid = nullptr;
            ee->visit(*this); // will call extrude_thing()
            gcode += visitor_gcode;
        }
    }
    return gcode;
}


void GCode::_post_process(std::string& what) {
    //if enabled, move the fan startup earlier.
    if (this->config().fan_speedup_time.value != 0) {
        FanMover fen_post_process(std::abs(this->config().fan_speedup_time.value), this->config().fan_speedup_time.value>0);
        what = fen_post_process.process_gcode(what);
    }
}

void GCode::_write(FILE* file, const char *what)
{
    if (what != nullptr) {
        
        //const char * gcode_pp = _post_process(what).c_str();
        std::string str_preproc{ what };
        //_post_process(str_preproc);

        const std::string str_ana = m_analyzer.process_gcode(str_preproc);

        // apply analyzer, if enabled
        const char * gcode = m_enable_analyzer ? str_ana.c_str() : str_preproc.c_str();

        // writes string to file
        fwrite(gcode, 1, ::strlen(gcode), file);
        // updates time estimator and gcode lines vector
        m_normal_time_estimator.add_gcode_block(gcode);
        if (m_silent_time_estimator_enabled)
            m_silent_time_estimator.add_gcode_block(gcode);
    }
}

void GCode::_writeln(FILE* file, const std::string &what)
{
    if (! what.empty())
        _write(file, (what.back() == '\n') ? what : (what + '\n'));
}

void GCode::_write_format(FILE* file, const char* format, ...)
{
    va_list args;
    va_start(args, format);

    int buflen;
    {
        va_list args2;
        va_copy(args2, args);
        buflen =
    #ifdef _MSC_VER
            ::_vscprintf(format, args2)
    #else
            ::vsnprintf(nullptr, 0, format, args2)
    #endif
            + 1;
        va_end(args2);
    }

    char buffer[1024];
    bool buffer_dynamic = buflen > 1024;
    char *bufptr = buffer_dynamic ? (char*)malloc(buflen) : buffer;
    int res = ::vsnprintf(bufptr, buflen, format, args);
    if (res > 0)
        _write(file, bufptr);

    if (buffer_dynamic)
        free(bufptr);

    va_end(args);
}

//external_perimeter_cut_corners cache, from 30deg to 145deg (115 deg)
std::vector<double> cut_corner_cache = {
    0.001537451157993,0.001699627500179,0.001873176359929,0.002058542095754,0.002256177154906,0.002466542444994,0.002690107718482,0.002927351970781,0.003178763852686,0.003444842097951,
    0.003726095966834,0.004023045706492,0.004336223029152,0.00466617160904,0.005013447599101,0.005378620168593,0.005762272062727,0.006165000185567,0.006587416207474,0.007030147198493,
    0.007493836289104,0.007979143359902,0.008486745761834,0.009017339068734,0.00957163786399,0.010150376563326,0.010754310275767,0.011384215705013,0.012040892093603,0.012725162212361,
    0.013437873397832,0.01417989864057,0.01495213772733,0.01575551844043,0.016590997817786,0.017459563477334,0.018362235009846,0.019300065444398,0.020274142791089,0.021285591665892,
    0.022335575002924,0.023425295859755,0.024555999321851,0.025728974512639,0.026945556716223,0.028207129620272,0.029515127687218,0.030871038662503,0.032276406229305,0.033732832819934,
    0.035241982594887,0.036805584601441,0.038425436124638,0.040103406244574,0.041841439615055,0.043641560479958,0.045505876945025,0.047436585524337,0.049435975982392,0.051506436494553,
    0.053650459150638,0.055870645828676,0.058169714468295,0.0605505057759,0.063015990396837,0.065569276592991,0.068213618467979,0.070952424786126,0.073789268435947,0.076727896593837,
    0.079772241649261,0.082926432958949,0.086194809504486,0.089581933535469,0.093092605289007,0.096731878886046,0.100505079515854,0.10441782203221,0.108476031098559,0.112685963034856,
    0.117054229536308,0.121587823453898,0.126294146848979,0.131181041559526,0.136256822544454,0.141530314305188,0.147010890721085,0.152708518678027,0.158633805918466,0.164798053597366,
    0.17121331409307,0.17789245469658,0.184849227888721,0.192098349014236,0.199655582277462,0.207537836118677,0.215763269187181,0.224351408310655,0.233323280075731,0.242701557887958,
    0.252510726678311,0.262777267777188,0.27352986689699,0.284799648665007,0.296620441746888,0.309029079319231,0.322065740515038,0.335774339512048,0.350202970204428,0.365404415947691,
    0.381436735764648,0.398363940736199,0.416256777189962,0.435193636891737,0.455261618934834 };

std::string GCode::_extrude(const ExtrusionPath &path, const std::string &description, double speed) {

    std::string descr = description + extrusion_role_2_string(path.role());
    std::string gcode = this->_before_extrude(path, descr, speed);
    
    // calculate extrusion length per distance unit
    double e_per_mm = path.mm3_per_mm
        * m_writer.tool()->e_per_mm3()
        * this->config().print_extrusion_multiplier.get_abs_value(1);
    if (this->m_layer_index <= 0) e_per_mm *= this->config().first_layer_flow_ratio.get_abs_value(1);
    if (m_writer.extrusion_axis().empty()) e_per_mm = 0;
    if (path.polyline.lines().size() > 0) {
        //get last direction //TODO: save it
        {
            std::string comment = m_config.gcode_comments ? descr : "";
            if (path.role() != erExternalPerimeter || config().external_perimeter_cut_corners.value == 0) {
                // normal & legacy pathcode
                for (const Line& line : path.polyline.lines()) {
                    if (line.a == line.b) continue; //todo: investigate if it happens (it happens in perimeters)
                    gcode += m_writer.extrude_to_xy(
                        this->point_to_gcode(line.b),
                        e_per_mm * unscaled(line.length()),
                        comment);
                }
            } else {
                // external_perimeter_cut_corners pathcode
                Point last_pos = path.polyline.lines()[0].a;
                for (const Line& line : path.polyline.lines()) {
                    if (line.a == line.b) continue; //todo: investigate if it happens (it happens in perimeters)
                    //check the angle
                    double angle = line.a == last_pos ? PI : line.a.ccw_angle(last_pos, line.b);
                    //convert the angle from the angle of the line to the angle of the "joint" (Circular segment)
                    if (angle > PI) angle = angle - PI;
                    else angle = PI - angle;
                    int idx_angle = int(180 * angle / PI);
                    // the coeff is below 0.01 i the angle is higher than 125, so it's not useful
                    if (idx_angle > 60) {
                        //don't compensate if the angle is under 35, as it's already a 50% compensation, it's enough! 
                        if (idx_angle > 144) angle = 144;
                        //surface extruded in path.width is path.width * path.width
                        // define R = path.width/2 and a = angle/2
                        // then i have to print only 4RR + RR(2a-sin(2a))/2 - RR*sina*sina*tana if i want to remove the bits out of the external curve, if the internal overlap go to the exterior.
                        // so over RR, i have to multiply the extrudion per 1 + (2a-sin(2a))/8 - (sina*sina*tana)/4
                        //double R = scale_(path.width) / 2;
                        //double A = (PI - angle) / 2;
                        //double added = (A - std::sin(A + A) / 2);
                        //double removed = std::sin(A); removed = removed * removed * std::tan(A) / 4;
                        //double coeff = 1. + added - removed;
                        //we have to remove coeff percentage on path.width length
                        double coeff = cut_corner_cache[idx_angle];
                        //the length, do half of the work on width/4 and the other half on width/2
                        coordf_t length1 = (path.width) / 4;
                        coordf_t line_length = unscaled(line.length());
                        if (line_length > length1) {
                            double mult1 = 1 - coeff * 2;
                            double length2 = (path.width) / 2;
                            double mult2 = 1 - coeff;
                            double sum = 0;
                            //Create a point
                            Point inter_point1 = line.point_at(scale_(length1));
                            //extrude very reduced
                            gcode += m_writer.extrude_to_xy(
                                this->point_to_gcode(inter_point1),
                                e_per_mm * (length1) * mult1,
                                comment);
                            sum += e_per_mm * (length1) * mult1;

                            if (line_length - length1 > length2) {
                                Point inter_point2 = line.point_at(scale_(length2));
                                //extrude reduced
                                gcode += m_writer.extrude_to_xy(
                                    this->point_to_gcode(inter_point2),
                                    e_per_mm * (length2) * mult2,
                                    comment);
                                sum += e_per_mm * (length2) * mult2;

                                //extrude normal
                                gcode += m_writer.extrude_to_xy(
                                    this->point_to_gcode(line.b),
                                    e_per_mm * (line_length - (length1 + length2)),
                                    comment);
                                sum += e_per_mm * (line_length - (length1 + length2));
                            } else {
                                mult2 = 1 - coeff * (length2 / (line_length - length1));
                                gcode += m_writer.extrude_to_xy(
                                    this->point_to_gcode(line.b),
                                    e_per_mm * (line_length - length1) * mult2,
                                    comment);
                                sum += e_per_mm * (line_length - length1) * mult2;
                            }
                        } else {
                            double mult = std::max(0.1, 1 - coeff * (scale_(path.width) / line_length));
                            gcode += m_writer.extrude_to_xy(
                                this->point_to_gcode(line.b),
                                e_per_mm * line_length * mult,
                                comment);
                        }
                    } else {
                        // nothing special, angle is too shallow to have any impact.
                        gcode += m_writer.extrude_to_xy(
                            this->point_to_gcode(line.b),
                            e_per_mm * unscaled(line.length()),
                            comment);
                    }

                    //relance
                    last_pos = line.a;
                }
            }
        }
    }
    gcode += this->_after_extrude(path);

    return gcode;
}

std::string GCode::_before_extrude(const ExtrusionPath &path, const std::string &description_in, double speed) {
    std::string gcode;
    std::string description{ description_in };
    if (is_bridge(path.role()))
        description += " (bridge)";
    
    // go to first point of extrusion path
    if (!m_last_pos_defined || m_last_pos != path.first_point()) {
        gcode += this->travel_to(
            path.first_point(),
            path.role(),
            "move to first " + description + " point"
        );
    }
    
    // compensate retraction
    gcode += this->unretract();
    
    // adjust acceleration
    {
        double acceleration;
        if (this->on_first_layer() && m_config.first_layer_acceleration.value > 0) {
            acceleration = m_config.first_layer_acceleration.value;
        } else if (m_config.perimeter_acceleration.value > 0 && is_perimeter(path.role())) {
            acceleration = m_config.perimeter_acceleration.value;
        } else if (m_config.bridge_acceleration.value > 0 && is_bridge(path.role())) {
            acceleration = m_config.bridge_acceleration.value;
        } else if (m_config.infill_acceleration.value > 0 && is_infill(path.role())) {
            acceleration = m_config.infill_acceleration.value;
        } else {
            acceleration = m_config.default_acceleration.value;
        }//TODO: add travel accel?
        m_writer.set_acceleration((unsigned int)floor(acceleration + 0.5));
    }


    // set speed
    if (speed == -1) {
        if (path.role() == erPerimeter) {
            speed = m_config.get_abs_value("perimeter_speed");
        } else if (path.role() == erExternalPerimeter) {
            speed = m_config.get_abs_value("external_perimeter_speed");
        } else if (path.role() == erOverhangPerimeter || path.role() == erBridgeInfill) {
            speed = m_config.get_abs_value("bridge_speed");
        } else if (path.role() == erInternalInfill) {
            speed = m_config.get_abs_value("infill_speed");
        } else if (path.role() == erSolidInfill) {
            speed = m_config.get_abs_value("solid_infill_speed");
        } else if (path.role() == erTopSolidInfill) {
            speed = m_config.get_abs_value("top_solid_infill_speed");
        } else if (path.role() == erThinWall) {
            speed = m_config.get_abs_value("thin_walls_speed");
        } else if (path.role() == erGapFill) {
            speed = m_config.get_abs_value("gap_fill_speed");
        } else if (path.role() == erNone) {
            speed = m_config.get_abs_value("travel_speed");
        } else if (path.role() == erMilling) {
            speed = m_config.get_abs_value("milling_speed");
        } else {
            throw std::invalid_argument("Invalid speed");
        }
    }
    if (m_volumetric_speed != 0. && speed == 0)
        speed = m_volumetric_speed / path.mm3_per_mm;
    if (speed == 0) // this code shouldn't trigger as if it's 0, you have to get a m_volumetric_speed
        speed = m_config.max_print_speed.value;
    if (this->on_first_layer())
        if (path.role() == erInternalInfill || path.role() == erSolidInfill)
            speed = std::min(m_config.get_abs_value("first_layer_infill_speed", speed), speed);
        else
            speed = std::min(m_config.get_abs_value("first_layer_speed", speed), speed);
    if (m_config.max_volumetric_speed.value > 0) {
        // cap speed with max_volumetric_speed anyway (even if user is not using autospeed)
        speed = std::min(
            speed,
            m_config.max_volumetric_speed.value / path.mm3_per_mm
            );
    }
    if (EXTRUDER_CONFIG_WITH_DEFAULT(filament_max_volumetric_speed,0) > 0) {
        // cap speed with max_volumetric_speed anyway (even if user is not using autospeed)
        speed = std::min(
            speed,
            EXTRUDER_CONFIG_WITH_DEFAULT(filament_max_volumetric_speed, speed) / path.mm3_per_mm
            );
    }
    double F = speed * 60;  // convert mm/sec to mm/min
    
    // extrude arc or line
    if (path.role() != m_last_extrusion_role && !m_config.feature_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("extrusion_role", new ConfigOptionString(extrusion_role_to_string_for_parser(path.role())));
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index + 1));
        config.set_key_value("layer_z", new ConfigOptionFloat(m_config.z_offset.value));
        gcode += this->placeholder_parser_process("feature_gcode",
            m_config.feature_gcode.value, m_writer.tool()->id(), &config)
            + "\n";
    }
    if (m_enable_extrusion_role_markers) {
        if (path.role() != m_last_extrusion_role) {
            char buf[32];
            sprintf(buf, ";_EXTRUSION_ROLE:%d\n", int(path.role()));
            gcode += buf;
        }
    }
    m_last_extrusion_role = path.role();

    // adds analyzer tags and updates analyzer's tracking data
    if (m_enable_analyzer)
    {
        // PrusaMultiMaterial::Writer may generate GCodeAnalyzer::Height_Tag and GCodeAnalyzer::Width_Tag lines without updating m_last_height and m_last_width
        // so, if the last role was erWipeTower we force export of GCodeAnalyzer::Height_Tag and GCodeAnalyzer::Width_Tag lines
        bool last_was_wipe_tower = (m_last_analyzer_extrusion_role == erWipeTower);
        char buf[64];

        if (path.role() != m_last_analyzer_extrusion_role)
        {
            m_last_analyzer_extrusion_role = path.role();
            sprintf(buf, ";%s%d\n", GCodeAnalyzer::Extrusion_Role_Tag.c_str(), int(m_last_analyzer_extrusion_role));
            gcode += buf;
        }

        if (last_was_wipe_tower || (m_last_mm3_per_mm != path.mm3_per_mm))
        {
            m_last_mm3_per_mm = path.mm3_per_mm;
            sprintf(buf, ";%s%f\n", GCodeAnalyzer::Mm3_Per_Mm_Tag.c_str(), m_last_mm3_per_mm);
            gcode += buf;
        }

        if (last_was_wipe_tower || (m_last_width != path.width))
        {
            m_last_width = path.width;
            sprintf(buf, ";%s%f\n", GCodeAnalyzer::Width_Tag.c_str(), m_last_width);
            gcode += buf;
        }

        if (last_was_wipe_tower || (m_last_height != path.height))
        {
            m_last_height = path.height;
            sprintf(buf, ";%s%f\n", GCodeAnalyzer::Height_Tag.c_str(), m_last_height);
            gcode += buf;
        }
    }

    std::string comment;
    if (m_enable_cooling_markers) {
        if (is_bridge(path.role()))
            gcode += ";_BRIDGE_FAN_START\n";
        else if (ExtrusionRole::erTopSolidInfill == path.role())
            gcode += ";_TOP_FAN_START\n";
        else
            comment = ";_EXTRUDE_SET_SPEED";
        if (path.role() == erExternalPerimeter)
            comment += ";_EXTERNAL_PERIMETER";
        if (path.role() == erThinWall)
            comment += ";_EXTERNAL_PERIMETER";
    }
    // F is mm per minute.
    gcode += m_writer.set_speed(F, "", comment);

    return gcode;
}
std::string GCode::_after_extrude(const ExtrusionPath &path) {
    std::string gcode;
    if (m_enable_cooling_markers)
        if (is_bridge(path.role()))
            gcode += ";_BRIDGE_FAN_END\n";
        else if (ExtrusionRole::erTopSolidInfill == path.role())
            gcode += ";_TOP_FAN_END\n";
        else
            gcode += ";_EXTRUDE_END\n";

    
    this->set_last_pos(path.last_point());
    return gcode;
}

// This method accepts &point in print coordinates.
std::string GCode::travel_to(const Point &point, ExtrusionRole role, std::string comment)
{    
    /*  Define the travel move as a line between current position and the taget point.
        This is expressed in print coordinates, so it will need to be translated by
        this->origin in order to get G-code coordinates.  */
    Polyline travel;
    travel.append(this->last_pos());
    travel.append(point);
    
    // check whether a straight travel move would need retraction
    bool needs_retraction = this->needs_retraction(travel, role);
    
    // if a retraction would be needed, try to use avoid_crossing_perimeters to plan a
    // multi-hop travel path inside the configuration space
    if (needs_retraction
        && m_config.avoid_crossing_perimeters
        && ! m_avoid_crossing_perimeters.disable_once
        && m_avoid_crossing_perimeters.is_init()
        && !(m_config.avoid_crossing_not_first_layer && this->on_first_layer())) {
        travel = m_avoid_crossing_perimeters.travel_to(*this, point);
        
        // check again whether the new travel path still needs a retraction
        needs_retraction = this->needs_retraction(travel, role);
        //if (needs_retraction && m_layer_index > 1) exit(0);
    }
    
    // Re-allow avoid_crossing_perimeters for the next travel moves
    m_avoid_crossing_perimeters.disable_once = false;
    m_avoid_crossing_perimeters.use_external_mp_once = false;
    
    // generate G-code for the travel move
    std::string gcode;
    if (needs_retraction)
        gcode += this->retract();
    else
        // Reset the wipe path when traveling, so one would not wipe along an old path.
        m_wipe.reset_path();
    
    // use G1 because we rely on paths being straight (G0 may make round paths)
    Lines lines = travel.lines();
    if (! lines.empty()) {
        for (const Line &line : lines)
    	    gcode += m_writer.travel_to_xy(this->point_to_gcode(line.b), comment);    
        this->set_last_pos(lines.back().b);
    }
    return gcode;
}

bool GCode::needs_retraction(const Polyline &travel, ExtrusionRole role)
{
    if (travel.length() < scale_(EXTRUDER_CONFIG_WITH_DEFAULT(retract_before_travel, 0))) {
        // skip retraction if the move is shorter than the configured threshold
        return false;
    }
    
    if (role == erSupportMaterial) {
        const SupportLayer* support_layer = dynamic_cast<const SupportLayer*>(m_layer);
        //FIXME support_layer->support_islands.contains should use some search structure!
        if (support_layer != NULL && support_layer->support_islands.contains(travel))
            // skip retraction if this is a travel move inside a support material island
            //FIXME not retracting over a long path may cause oozing, which in turn may result in missing material
            // at the end of the extrusion path!
            return false;
    }

    if (m_config.only_retract_when_crossing_perimeters && m_layer != nullptr &&
        m_config.fill_density.value > 0 && m_layer->any_internal_region_slice_contains(travel))
        // Skip retraction if travel is contained in an internal slice *and*
        // internal infill is enabled (so that stringing is entirely not visible).
        //FIXME any_internal_region_slice_contains() is potentionally very slow, it shall test for the bounding boxes first.
        return false;
    
    // retract if only_retract_when_crossing_perimeters is disabled or doesn't apply
    return true;
}

std::string GCode::retract(bool toolchange)
{
    std::string gcode;
    
    if (m_writer.tool() == nullptr)
        return gcode;

    // We need to reset e before any extrusion or wipe to allow the reset to happen at the real 
    // begining of an object gcode
    gcode += m_writer.reset_e();
    
    // wipe (if it's enabled for this extruder and we have a stored wipe path)
    if (BOOL_EXTRUDER_CONFIG(wipe) && m_wipe.has_path()) {
        gcode += toolchange ? m_writer.retract_for_toolchange(true) : m_writer.retract(true);
        gcode += m_wipe.wipe(*this, toolchange);
    }
    
    /*  The parent class will decide whether we need to perform an actual retraction
        (the extruder might be already retracted fully or partially). We call these 
        methods even if we performed wipe, since this will ensure the entire retraction
        length is honored in case wipe path was too short.  */
    gcode += toolchange ? m_writer.retract_for_toolchange() : m_writer.retract();
    if (toolchange 
        || !(BOOL_EXTRUDER_CONFIG(retract_lift_not_last_layer) && (this->m_last_extrusion_role == ExtrusionRole::erTopSolidInfill ))
        || !m_writer.tool_is_extruder()
        )
        if (m_writer.tool()->retract_length() > 0 
            || m_config.use_firmware_retraction 
            || (!m_writer.tool_is_extruder() && m_writer.tool()->retract_lift() != 0)
            )
            gcode += m_writer.lift();
    
    return gcode;
}

std::string GCode::set_extruder(unsigned int extruder_id, double print_z, bool no_toolchange /*=false*/)
{
    if (!m_writer.need_toolchange(extruder_id))
        return "";
    
    // if we are running a single-extruder setup, just set the extruder and return nothing
    if (!m_writer.multiple_extruders) {
        m_placeholder_parser.set("current_extruder", extruder_id);
        
        std::string gcode;
        // Append the filament start G-code.
        const std::string &start_filament_gcode = m_config.start_filament_gcode.get_at(extruder_id);
        if (! start_filament_gcode.empty()) {
            DynamicConfig config;
            config.set_key_value("previous_extruder", new ConfigOptionInt((int)extruder_id));
            config.set_key_value("next_extruder", new ConfigOptionInt((int)extruder_id));
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
            // Process the start_filament_gcode for the new filament.
            gcode += this->placeholder_parser_process("start_filament_gcode", start_filament_gcode, extruder_id, &config);
            check_add_eol(gcode);
        }
        gcode += m_writer.toolchange(extruder_id);
        return gcode;
    }
    
    // prepend retraction on the current extruder
    std::string gcode = this->retract(true);

    // Always reset the extrusion path, even if the tool change retract is set to zero.
    m_wipe.reset_path();
    
    if (m_writer.tool() != nullptr) {
        // Process the custom end_filament_gcode. set_extruder() is only called if there is no wipe tower
        // so it should not be injected twice.
        unsigned int        old_extruder_id     = m_writer.tool()->id();
        const std::string  &end_filament_gcode  = m_config.end_filament_gcode.get_at(old_extruder_id);
        if (! end_filament_gcode.empty()) {
            DynamicConfig config;
            config.set_key_value("previous_extruder", new ConfigOptionInt((int)(m_writer.tool() != nullptr ? m_writer.tool()->id() : -1)));
            config.set_key_value("next_extruder", new ConfigOptionInt((int)extruder_id));
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
            gcode += placeholder_parser_process("end_filament_gcode", end_filament_gcode, old_extruder_id, &config);
            check_add_eol(gcode);
        }
    }

    
    // If ooze prevention is enabled, park current extruder in the nearest
    // standby point and set it to the standby temperature.
    if (m_ooze_prevention.enable && m_writer.tool() != nullptr)
        gcode += m_ooze_prevention.pre_toolchange(*this);

    const std::string& toolchange_gcode = m_config.toolchange_gcode.value;
    std::string toolchange_gcode_parsed;

    // Process the custom toolchange_gcode. If it is empty, insert just a Tn command.
    if (!toolchange_gcode.empty()) {
        DynamicConfig config;
        config.set_key_value("previous_extruder", new ConfigOptionInt((int)(m_writer.tool() != nullptr ? m_writer.tool()->id() : -1 )));
        config.set_key_value("next_extruder",     new ConfigOptionInt((int)extruder_id));
        config.set_key_value("layer_num",         new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z",           new ConfigOptionFloat(print_z));
        toolchange_gcode_parsed = placeholder_parser_process("toolchange_gcode", toolchange_gcode, extruder_id, &config);
        gcode += toolchange_gcode_parsed;
        check_add_eol(gcode);
    }

    // We inform the writer about what is happening, but we may not use the resulting gcode.
    std::string toolchange_command = m_writer.toolchange(extruder_id);
    if (! custom_gcode_changes_tool(toolchange_gcode_parsed, m_writer.toolchange_prefix(), extruder_id) && !no_toolchange)
        gcode += toolchange_command;
    else {
        // user provided his own toolchange gcode, no need to do anything
    }

    // Set the temperature if the wipe tower didn't (not needed for non-single extruder MM)
    if (m_config.single_extruder_multi_material && !m_config.wipe_tower) {
        int temp = (m_layer_index <= 0 ? m_config.first_layer_temperature.get_at(extruder_id) :
                                         m_config.temperature.get_at(extruder_id));

        gcode += m_writer.set_temperature(temp, false);
    }

    m_placeholder_parser.set("current_extruder", extruder_id);

    // Append the filament start G-code.
    const std::string &start_filament_gcode = m_config.start_filament_gcode.get_at(extruder_id);
    if (! start_filament_gcode.empty()) {
        DynamicConfig config;
        config.set_key_value("previous_extruder", new ConfigOptionInt((int)(m_writer.tool() != nullptr ? m_writer.tool()->id() : -1)));
        config.set_key_value("next_extruder", new ConfigOptionInt((int)extruder_id));
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
        // Process the start_filament_gcode for the new filament.
        gcode += this->placeholder_parser_process("start_filament_gcode", start_filament_gcode, extruder_id, &config);
        check_add_eol(gcode);
    }
    // Set the new extruder to the operating temperature.
    if (m_ooze_prevention.enable)
        gcode += m_ooze_prevention.post_toolchange(*this);
    
    return gcode;
}

// convert a model-space scaled point into G-code coordinates
Vec2d GCode::point_to_gcode(const Point &point) const
{
    Vec2d extruder_offset = EXTRUDER_CONFIG_WITH_DEFAULT(extruder_offset, Vec2d(0, 0)); //FIXME : mill ofsset
    return unscale(point) + m_origin - extruder_offset;
}

// convert a model-space scaled point into G-code coordinates
Vec3d GCode::point_to_gcode(const Point &point, const coord_t z_offset) const {
    Vec2d extruder_offset = EXTRUDER_CONFIG_WITH_DEFAULT(extruder_offset, Vec2d(0, 0)); //FIXME : mill ofsset
    Vec3d ret_vec(unscale_(point.x()) + m_origin.x() - extruder_offset.x(),
        unscale_(point.y()) + m_origin.y() - extruder_offset.y(),
        unscale_(z_offset));
    return ret_vec;
}

// convert a model-space scaled point into G-code coordinates
Point GCode::gcode_to_point(const Vec2d &point) const
{
    Vec2d extruder_offset = EXTRUDER_CONFIG_WITH_DEFAULT(extruder_offset, Vec2d(0, 0)); //FIXME : mill ofsset
    return Point(
        scale_(point(0) - m_origin(0) + extruder_offset(0)),
        scale_(point(1) - m_origin(1) + extruder_offset(1)));
}

// Goes through by_region std::vector and returns reference to a subvector of entities, that are to be printed
// during infill/perimeter wiping, or normally (depends on wiping_entities parameter)
// Fills in by_region_per_copy_cache and returns its reference.
const std::vector<GCode::ObjectByExtruder::Island::Region>& GCode::ObjectByExtruder::Island::by_region_per_copy(std::vector<Region> &by_region_per_copy_cache, unsigned int copy, unsigned int extruder, bool wiping_entities) const
{
    bool has_overrides = false;
    for (const auto& reg : by_region)
    	if (! reg.infills_overrides.empty() || ! reg.perimeters_overrides.empty()) {
    		has_overrides = true;
    		break;
    	}

	// Data is cleared, but the memory is not.
    by_region_per_copy_cache.clear();

    if (! has_overrides)
    	// Simple case. No need to copy the regions.
    	return wiping_entities ? by_region_per_copy_cache : this->by_region;

    // Complex case. Some of the extrusions of some object instances are to be printed first - those are the wiping extrusions.
    // Some of the extrusions of some object instances are printed later - those are the clean print extrusions.
    // Filter out the extrusions based on the infill_overrides / perimeter_overrides:

    for (const auto& reg : by_region) {
        by_region_per_copy_cache.emplace_back(); // creates a region in the newly created Island

        // Now we are going to iterate through perimeters and infills and pick ones that are supposed to be printed
        // References are used so that we don't have to repeat the same code
        for (int iter = 0; iter < 2; ++iter) {
            const ExtrusionEntitiesPtr&										entities    = (iter ? reg.infills : reg.perimeters);
            ExtrusionEntitiesPtr&   										target_eec  = (iter ? by_region_per_copy_cache.back().infills : by_region_per_copy_cache.back().perimeters);
            const std::vector<const WipingExtrusions::ExtruderPerCopy*>& 	overrides   = (iter ? reg.infills_overrides : reg.perimeters_overrides);

            // Now the most important thing - which extrusion should we print.
            // See function ToolOrdering::get_extruder_overrides for details about the negative numbers hack.
            if (wiping_entities) {
            	// Apply overrides for this region.
	            for (unsigned int i = 0; i < overrides.size(); ++ i) {
            		const WipingExtrusions::ExtruderPerCopy *this_override = overrides[i];
            		// This copy (aka object instance) should be printed with this extruder, which overrides the default one.
	                if (this_override != nullptr && (*this_override)[copy] == int(extruder))
	                    target_eec.emplace_back(entities[i]);
            	}
	        } else {
	        	// Apply normal extrusions (non-overrides) for this region.
				unsigned int i = 0;
	            for (; i < overrides.size(); ++ i) {
            		const WipingExtrusions::ExtruderPerCopy *this_override = overrides[i];
            		// This copy (aka object instance) should be printed with this extruder, which shall be equal to the default one.
            		if (this_override == nullptr || (*this_override)[copy] == -int(extruder)-1)
	                    target_eec.emplace_back(entities[i]);
	            }
	            for (; i < entities.size(); ++ i)
                    target_eec.emplace_back(entities[i]);
		    }
        }
    }
    return by_region_per_copy_cache;
}

// This function takes the eec and appends its entities to either perimeters or infills of this Region (depending on the first parameter)
// It also saves pointer to ExtruderPerCopy struct (for each entity), that holds information about which extruders should be used for which copy.
void GCode::ObjectByExtruder::Island::Region::append(const Type type, const ExtrusionEntityCollection* eec, const WipingExtrusions::ExtruderPerCopy* copies_extruder)
{
    // We are going to manipulate either perimeters or infills, exactly in the same way. Let's create pointers to the proper structure to not repeat ourselves:
    ExtrusionEntitiesPtr*									perimeters_or_infills;
    std::vector<const WipingExtrusions::ExtruderPerCopy*>* 	perimeters_or_infills_overrides;

    switch (type) {
    case PERIMETERS:
        perimeters_or_infills 			= &perimeters;
        perimeters_or_infills_overrides = &perimeters_overrides;
        break;
    case INFILL:
    	perimeters_or_infills 			= &infills;
    	perimeters_or_infills_overrides = &infills_overrides;
        break;
    default:
    	throw std::invalid_argument("Unknown parameter!");
    }

    // First we append the entities, there are eec->entities.size() of them:
    //don't do fill->entities because it will discard no_sort, we must use flatten(preserve_ordering = true)
    ExtrusionEntitiesPtr entities = eec->flatten(true).entities;
    size_t old_size = perimeters_or_infills->size();
    size_t new_size = old_size + entities.size();
    perimeters_or_infills->reserve(new_size);
    for (auto* ee : entities)
        perimeters_or_infills->emplace_back(ee);

    if (copies_extruder != nullptr) {
    	// Don't reallocate overrides if not needed.
    	// Missing overrides are implicitely considered non-overridden.
        perimeters_or_infills_overrides->reserve(new_size);
        perimeters_or_infills_overrides->resize(old_size, nullptr);
        perimeters_or_infills_overrides->resize(new_size, copies_extruder);
	}
}

std::string
GCode::extrusion_role_to_string_for_parser(const ExtrusionRole & role) {
    switch (role) {
    case erPerimeter:
        return "Perimeter";
    case erExternalPerimeter:
        return "ExternalPerimeter";
    case erOverhangPerimeter:
        return "OverhangPerimeter";
    case erInternalInfill:
        return "InternalInfill";
    case erSolidInfill:
        return "SolidInfill";
    case erTopSolidInfill:
        return "TopSolidInfill";
    case erBridgeInfill:
        return "BridgeInfill";
    case erThinWall:
        return "ThinWall";
    case erGapFill:
        return "GapFill";
    case erSkirt:
        return "Skirt";
    case erSupportMaterial:
        return "SupportMaterial";
    case erSupportMaterialInterface:
        return "SupportMaterialInterface";
    case erWipeTower:
        return "WipeTower";
    case erMilling:
        return "Mill";
    case erCustom:
    case erMixed:
    case erCount:
    case erNone:
    default:
        return "Mixed";
    }
}

}   // namespace Slic3r
