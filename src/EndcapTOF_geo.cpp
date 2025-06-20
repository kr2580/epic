// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2022 - 2024, Nicolas Schmidt, Chun Yuen Tsang

/** \addtogroup Trackers Trackers
 * \brief Type: **Endcap Tracker with TOF**.
 *
 * \ingroup trackers
 *
 * @{
 */
#include "DD4hep/DetFactoryHelper.h"
#include "DD4hep/Printout.h"
#include "DD4hep/Shapes.h"
#include "DD4hepDetectorHelper.h"
#include "DDRec/DetectorData.h"
#include "DDRec/Surface.h"
#include "XML/Layering.h"
#include "XML/Utilities.h"
#include <array>
#include <map>
#include <unordered_set>

using namespace std;
using namespace dd4hep;
using namespace dd4hep::rec;
using namespace dd4hep::detail;

static Ref_t create_detector(Detector& description, xml_h e, SensitiveDetector sens) {
  xml_det_t x_det      = e;
  int det_id           = x_det.id();
  std::string det_name = x_det.nameStr();
  DetElement sdet(det_name, det_id);
  Material air    = description.material("Air");
  Material carbon = description.material("CarbonFiber");
  PlacedVolume pv;

  map<string, std::array<double, 2>> module_thicknesses;

  // Set detector type flag
  dd4hep::xml::setDetectorTypeFlag(x_det, sdet);
  auto& params = DD4hepDetectorHelper::ensureExtension<dd4hep::rec::VariantParameters>(sdet);
  // Add the volume boundary material if configured
  for (xml_coll_t bmat(x_det, _Unicode(boundary_material)); bmat; ++bmat) {
    xml_comp_t x_boundary_material = bmat;
    DD4hepDetectorHelper::xmlToProtoSurfaceMaterial(x_boundary_material, params,
                                                    "boundary_material");
  }

  Assembly assembly(det_name);
  assembly.setVisAttributes(description.invisible());
  sens.setType("tracker");

  float zPos = 0;
  // now build the envelope for the detector
  xml_comp_t x_layer  = x_det.child(_Unicode(layer));
  xml_comp_t envelope = x_layer.child(_Unicode(envelope), false);
  int lay_id          = x_layer.id();
  string l_nam        = x_layer.moduleStr();
  string lay_nam      = det_name + _toString(x_layer.id(), "_layer%d");
  Tube lay_tub(envelope.rmin(), envelope.rmax(), envelope.length() / 2.0);
  Volume lay_vol(lay_nam, lay_tub, air); // Create the layer envelope volume.
  zPos = envelope.zstart();
  Position lay_pos(0, 0, 0);
  lay_vol.setVisAttributes(description.visAttributes(x_layer.visStr()));

  DetElement lay_elt(sdet, lay_nam, lay_id);

  // the local coordinate systems of modules in dd4hep and acts differ
  // see http://acts.web.cern.ch/ACTS/latest/doc/group__DD4hepPlugins.html
  auto& layerParams =
      DD4hepDetectorHelper::ensureExtension<dd4hep::rec::VariantParameters>(lay_elt);

  for (xml_coll_t lmat(x_layer, _Unicode(layer_material)); lmat; ++lmat) {
    xml_comp_t x_layer_material = lmat;
    DD4hepDetectorHelper::xmlToProtoSurfaceMaterial(x_layer_material, layerParams,
                                                    "layer_material");
  }

  // dimensions of the modules (2x2 sensors)
  xml_comp_t x_modsz = x_det.child(_Unicode(modsize));

  double module_x       = x_modsz.length();
  double module_y       = x_modsz.width();
  double module_overlap = getAttrOrDefault(x_modsz, _Unicode(overlap), 0.); // x_modsz.overlap();
  double module_spacing = getAttrOrDefault(x_modsz, _Unicode(spacing), 0.); // x_modsz.overlap();
  double board_gap      = getAttrOrDefault(x_modsz, _Unicode(board_gap), 0.);

  //! Add support structure
  xml_comp_t x_supp          = x_det.child(_Unicode(support));
  xml_comp_t x_supp_envelope = x_supp.child(_Unicode(envelope), false);

  xml_comp_t x_modFrontLeft  = x_det.child(_Unicode(moduleFrontLeft));
  xml_comp_t x_modFrontRight = x_det.child(_Unicode(moduleFrontRight));
  xml_comp_t x_modBackLeft   = x_det.child(_Unicode(moduleBackLeft));
  xml_comp_t x_modBackRight  = x_det.child(_Unicode(moduleBackRight));

  xml_comp_t x_sensor_layout_front_left  = x_det.child(_Unicode(sensor_layout_front_left));
  xml_comp_t x_sensor_layout_back_left   = x_det.child(_Unicode(sensor_layout_back_left));
  xml_comp_t x_sensor_layout_front_right = x_det.child(_Unicode(sensor_layout_front_right));
  xml_comp_t x_sensor_layout_back_right  = x_det.child(_Unicode(sensor_layout_back_right));

  for (bool left : std::vector<bool>{true, false}) {
    for (bool front : std::vector<bool>{true, false}) {
      int module = (front << 1) + left;
      const std::string locStr =
          std::string(left ? "Left" : "Right") + std::string(front ? "Front" : "Back");
      float ycoord = envelope.rmax() -
                     module_y / 2.; // y-center-coord of the top sensor. Start from the top row
      int iy                     = 0;
      xml_comp_t x_sensor_layout = x_sensor_layout_front_left;
      xml_comp_t x_modCurr       = x_modFrontLeft;

      if (front) {
        if (left) {
          x_sensor_layout = x_sensor_layout_front_left;
          x_modCurr       = x_modFrontLeft;
        } else {
          x_sensor_layout = x_sensor_layout_front_right;
          x_modCurr       = x_modFrontRight;
        }
      } else {
        if (left) {
          x_sensor_layout = x_sensor_layout_back_left;
          x_modCurr       = x_modBackLeft;
        } else {
          x_sensor_layout = x_sensor_layout_back_right;
          x_modCurr       = x_modBackRight;
        }
      }

      double total_thickness = 0;
      // Compute module total thickness from components
      xml_coll_t ci(x_modCurr, _U(module_component));

      for (ci.reset(), total_thickness = 0.0; ci; ++ci) {
        xml_comp_t x_comp    = ci;
        bool keep_same_layer = getAttrOrDefault<bool>(x_comp, _Unicode(keep_layer), false);
        if (!keep_same_layer)
          total_thickness += x_comp.thickness();
      }

      for (xml_coll_t lrow(x_sensor_layout, _Unicode(row)); lrow; ++lrow) {
        xml_comp_t x_row = lrow;
        double deadspace = getAttrOrDefault<double>(x_row, _Unicode(deadspace), 0);
        if (deadspace > 0) {
          ycoord -= deadspace;
          continue;
        }
        double x_offset = getAttrOrDefault<double>(x_row, _Unicode(x_offset), 0);
        int nsensors    = getAttrOrDefault<int>(x_row, _Unicode(nsensors), 0);

        // find the sensor id that corrsponds to the rightmost sensor in a board
        // we need to know where to apply additional spaces between neighboring board
        std::unordered_set<int> sensors_id_board_edge;
        int curr_ix = nsensors; // the first sensor to the right of center has ix of nsensors
        for (xml_coll_t lboard(x_row, _Unicode(board)); lboard; ++lboard) {
          xml_comp_t x_board = lboard;
          int nboard_sensors = getAttrOrDefault<int>(x_board, _Unicode(nsensors), 1);
          curr_ix += nboard_sensors;
          sensors_id_board_edge.insert(curr_ix);
          sensors_id_board_edge.insert(2 * nsensors - curr_ix -
                                       1); // reflected to sensor id on the left
        }

        double accum_xoffset = x_offset;
        for (int ix = (left ? nsensors - 1 : nsensors); (ix >= 0) && (ix < 2 * nsensors);
             ix     = ix + (left ? -1 : 1)) {
          // add board spacing
          if (sensors_id_board_edge.find(ix) != sensors_id_board_edge.end())
            accum_xoffset = accum_xoffset + board_gap;

          // there is a hole in the middle, with radius = x_offset
          float xcoord = (ix - nsensors + 0.5) * (module_x + module_spacing) +
                         +(left ? -accum_xoffset : accum_xoffset);
          //! Note the module ordering is different for front and back side

          double module_z = x_supp_envelope.length() / 2.0 + total_thickness / 2;
          if (front)
            module_z *= -1;

          string module_name = Form("module%d_%d_%d", module, ix, iy);
          DetElement mod_elt(lay_elt, module_name, module);

          // create individual sensor layers here
          string m_nam = Form("EndcapTOF_Module%d_%d_%d", module, ix, iy);

          int ncomponents = 0;
          // the module assembly volume
          Assembly m_vol(m_nam);
          m_vol.setVisAttributes(description.visAttributes(x_modCurr.visStr()));

          double thickness_so_far     = 0.0;
          double thickness_sum        = -total_thickness / 2.0;
          double thickness_carbonsupp = 0.0;
          int sensitive_id            = 0;
          for (xml_coll_t mci(x_modCurr, _U(module_component)); mci; ++mci, ++ncomponents) {
            xml_comp_t x_comp = mci;
            xml_comp_t x_pos  = x_comp.position(false);
            xml_comp_t x_rot  = x_comp.rotation(false);
            const string c_nam =
                Form("component_%s_%s_ix%d_iy%d", x_comp.nameStr().c_str(), locStr.c_str(), ix, iy);

            Box c_box(x_comp.width() / 2, x_comp.length() / 2, x_comp.thickness() / 2);
            Volume c_vol(c_nam, c_box, description.material(x_comp.materialStr()));
            if (x_comp.materialStr() == "CarbonFiber") {
              thickness_carbonsupp = x_comp.thickness();
            }
            // Utility variable for the relative z-offset based off the previous components
            const double zoff = thickness_sum + x_comp.thickness() / 2.0;
            if (x_pos && x_rot) {
              Position c_pos(x_pos.x(0), x_pos.y(0), x_pos.z(0) + zoff);
              RotationZYX c_rot(x_rot.z(0), x_rot.y(0), x_rot.x(0));
              pv = m_vol.placeVolume(c_vol, Transform3D(c_rot, c_pos));
            } else if (x_rot) {
              Position c_pos(0, 0, zoff);
              pv = m_vol.placeVolume(
                  c_vol, Transform3D(RotationZYX(x_rot.z(0), x_rot.y(0), x_rot.x(0)), c_pos));
            } else if (x_pos) {
              pv = m_vol.placeVolume(c_vol, Position(x_pos.x(0), x_pos.y(0), x_pos.z(0) + zoff));
            } else {
              pv = m_vol.placeVolume(c_vol, Position(0, 0, zoff));
            }
            c_vol.setRegion(description, x_comp.regionStr());
            c_vol.setLimitSet(description, x_comp.limitsStr());
            c_vol.setVisAttributes(description, x_comp.visStr());
            if (x_comp.isSensitive()) {
              pv.addPhysVolID("idx", ix);
              pv.addPhysVolID("idy", iy);
              pv.addPhysVolID("ids", sensitive_id);
              ++sensitive_id;

              c_vol.setSensitiveDetector(sens);
              module_thicknesses[m_nam] = {thickness_so_far + x_comp.thickness() / 2.0,
                                           total_thickness - thickness_so_far -
                                               x_comp.thickness() / 2.0};

              // -------- create a measurement plane for the tracking surface attched to the sensitive volume -----
              Vector3D u(-1., 0., 0.);
              Vector3D v(0., -1., 0.);
              Vector3D n(0., 0., 1.);

              // compute the inner and outer thicknesses that need to be assigned to the tracking surface
              // depending on wether the support is above or below the sensor
              double inner_thickness = module_thicknesses[m_nam][0];
              double outer_thickness = module_thicknesses[m_nam][1];

              SurfaceType type(SurfaceType::Sensitive);

              VolPlane surf(c_vol, type, inner_thickness, outer_thickness, u, v, n);

              DetElement comp_de(mod_elt,
                                 std::string("de_") + pv.volume().name() + "_" +
                                     std::to_string(sensitive_id),
                                 module);
              comp_de.setPlacement(pv);

              auto& comp_de_params =
                  DD4hepDetectorHelper::ensureExtension<dd4hep::rec::VariantParameters>(comp_de);
              comp_de_params.set<string>("axis_definitions", "XYZ");
              volSurfaceList(comp_de)->push_back(surf);

              //--------------------------------------------
            }
            bool keep_same_layer = getAttrOrDefault<bool>(x_comp, _Unicode(keep_layer), false);
            if (!keep_same_layer) {
              thickness_sum += x_comp.thickness();
              thickness_so_far += x_comp.thickness();
              // apply relative offsets in z-position used to stack components side-by-side
              if (x_pos) {
                thickness_sum += x_pos.z(0);
                thickness_so_far += x_pos.z(0);
              }
            }
          }

          if (front) {
            // only draw support bar on one side
            // if you draw on both sides, they may overlap
            const string suppb_nam =
                Form("suppbar_%d_%d", ix, iy); //_toString(ncomponents, "component%d");
            Box suppb_box((module_x + module_spacing) / 2, thickness_carbonsupp / 2,
                          x_supp_envelope.length() / 2);
            Volume suppb_vol(suppb_nam, suppb_box, carbon);
            Transform3D trsupp(RotationZYX(0, 0, 0),
                               Position(xcoord, ycoord + module_y / 2 - module_overlap / 2, 0));
            suppb_vol.setVisAttributes(description, "AnlGray");

            pv = lay_vol.placeVolume(suppb_vol, trsupp);
          }
          // module built!

          Transform3D tr(RotationZYX(M_PI / 2, 0, 0), Position(xcoord, ycoord, module_z));

          pv = lay_vol.placeVolume(m_vol, tr);
          pv.addPhysVolID("module", module);
          mod_elt.setPlacement(pv);
        }
        ycoord -= (module_y - module_overlap);
        ++iy;
      }
    }
  }
  // Create the PhysicalVolume for the layer.
  pv = assembly.placeVolume(lay_vol, lay_pos); // Place layer in mother
  pv.addPhysVolID("layer", lay_id);            // Set the layer ID.
  lay_elt.setAttributes(description, lay_vol, x_layer.regionStr(), x_layer.limitsStr(),
                        x_layer.visStr());
  lay_elt.setPlacement(pv);

  pv = description.pickMotherVolume(sdet).placeVolume(assembly, Position(0, 0, zPos));
  pv.addPhysVolID("system", det_id);
  sdet.setPlacement(pv);

  return sdet;
}

//@}
// clang-format off
DECLARE_DETELEMENT(epic_TOFEndcap, create_detector)
