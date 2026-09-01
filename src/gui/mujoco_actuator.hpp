#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ava_actuator
{
constexpr double pi = 3.14159265358979323846;
struct Config {
        int    polePairs = 7, slots = 12, encoderBits = 16;
        double ratio = 9, gapMm = 0.5, eccentricMm = 0.05, axialMm = 0.1;
        double tiltDeg = 0.2, zeroDeg = 0, rotorInertia = 0.00002;
        double loadInertia = 0.002, motorTorque = 1.5, efficiency = 0.9;
        bool   exploded = true;
};
struct Stage {
        int    sun, planet, ring;
        double ratio() const { return 1.0 + double(ring) / sun; }
};
struct Model {
        std::string        xml;
        std::vector<Stage> stages;
        double             ratio = 1;
};
inline void
validate(const Config &c)
{
        auto range = [](double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; };
        if (c.polePairs < 1 || c.polePairs > 32 || c.slots < 3 || c.slots > 96 || c.encoderBits < 8 || c.encoderBits > 24 ||
            !range(c.ratio, 2.5, 100) || !range(c.gapMm, 0.05, 3) || !range(c.eccentricMm, 0, 1) || !range(c.axialMm, -1, 1) ||
            c.gapMm + c.axialMm <= 0.02 || !range(c.tiltDeg, -5, 5) || !range(c.zeroDeg, -180, 180) ||
            !range(c.rotorInertia, 1e-7, 0.01) || !range(c.loadInertia, 1e-5, 10) || !range(c.motorTorque, 0.01, 100) ||
            !range(c.efficiency, 0.1, 1))
                throw std::runtime_error("Invalid actuator parameters / 关节参数超出范围，或编码器实际气隙过小");
}
inline std::vector<Stage>
gears(double target)
{
        int                count = target <= 8 ? 1 : target <= 64 ? 2 : 3;
        std::vector<Stage> candidates;
        for (int sun = 12; sun <= 30; ++sun)
                for (int planet = 6; planet <= 36; ++planet) {
                        Stage s{sun, planet, sun + 2 * planet};
                        if (s.ring <= 96 && (s.ring + s.sun) % 3 == 0 && s.ratio() >= 2.5 && s.ratio() <= 8)
                                candidates.push_back(s);
                }
        const double desired = std::pow(target, 1.0 / count);
        std::stable_sort(candidates.begin(), candidates.end(), [&](auto a, auto b) {
                return std::abs(a.ratio() - desired) < std::abs(b.ratio() - desired);
        });
        if (count == 3)
                candidates.resize(std::min<size_t>(32, candidates.size()));
        double                      best = 1e9;
        std::vector<Stage>          selected, current;
        std::function<void(double)> search = [&](double product) {
                if (current.size() == count) {
                        double error = std::abs(product - target);
                        if (error < best) {
                                best     = error;
                                selected = current;
                        }
                        return;
                }
                for (auto s : candidates) {
                        current.push_back(s);
                        search(product * s.ratio());
                        current.pop_back();
                }
        };
        search(1);
        if (best / target > 0.005)
                throw std::runtime_error("No compatible tooth combination within 0.5% / 无法在0.5%误差内生成齿数");
        return selected;
}
/**
 * @brief An explicit, approximate magnetic encoder error model, independent of MuJoCo dynamics
 */
inline double
encoder(const Config &c, double q)
{
        double effectiveGap = c.gapMm + c.axialMm;
        double amplitude    = c.eccentricMm / 18.0 * (1 + effectiveGap / 2.0);
        double tilt         = c.tiltDeg * pi / 180;
        double angle        = q + c.zeroDeg * pi / 180 + amplitude * std::sin(q) + 0.25 * tilt * tilt * std::sin(2 * q);
        double step         = 2 * pi / std::ldexp(1.0, c.encoderBits);
        return std::round(angle / step) * step;
}
inline double
encoderVelocity(const Config &c, double q, double dq)
{
        double amplitude = c.eccentricMm / 18.0 * (1 + (c.gapMm + c.axialMm) / 2.0);
        double tilt      = c.tiltDeg * pi / 180;
        return dq * (1 + amplitude * std::cos(q) + 0.5 * tilt * tilt * std::cos(2 * q));
}
inline Model
generate(const Config &c)
{
        validate(c);
        Model result;
        result.stages = gears(c.ratio);
        for (auto s : result.stages)
                result.ratio *= s.ratio();
        const double       outputZ = .105 + (result.stages.size() - 1) * (c.exploded ? .065 : .025);
        std::ostringstream x;
        x.imbue(std::locale::classic());
        x << std::setprecision(12);
        x << "<mujoco model=\"PMSM_planetary_encoder_actuator\"><compiler angle=\"radian\"/>"
          << "<option timestep=\"0.001\" gravity=\"0 0 0\" integrator=\"implicitfast\"/>"
          << "<statistic center=\"0 0 " << (outputZ - .035) / 2 << "\" extent=\"" << (outputZ + .05) << "\"/>"
          << "<default><geom contype=\"0\" conaffinity=\"0\" density=\"1000\"/></default><custom>";
        auto numeric = [&](const std::string &name, double v) {
                x << "<numeric name=\"" << name << "\" data=\"" << v << "\"/>";
        };
        numeric("ava_actuator_version", 1);
        numeric("ava_ratio", result.ratio);
        numeric("ava_poles", c.polePairs);
        numeric("ava_slots", c.slots);
        numeric("ava_bits", c.encoderBits);
        numeric("ava_gap", c.gapMm);
        numeric("ava_eccentric", c.eccentricMm);
        numeric("ava_axial", c.axialMm);
        numeric("ava_tilt", c.tiltDeg);
        numeric("ava_zero", c.zeroDeg);
        numeric("ava_requested_ratio", c.ratio);
        numeric("ava_rotor_inertia", c.rotorInertia);
        numeric("ava_load_inertia", c.loadInertia);
        numeric("ava_motor_torque", c.motorTorque);
        numeric("ava_efficiency", c.efficiency);
        numeric("ava_exploded", c.exploded);
        numeric("ava_stages", static_cast<double>(result.stages.size()));
        for (size_t i = 0; i < result.stages.size(); ++i) {
                numeric("ava_sun_" + std::to_string(i), result.stages[i].sun);
                numeric("ava_planet_" + std::to_string(i), result.stages[i].planet);
        }
        x << "</custom><worldbody><light pos=\"0.3 -0.4 0.8\" dir=\"-0.3 0.4 -0.8\"/>"
          << "<light pos=\"-0.3 0.2 0.5\" diffuse=\"0.5 0.5 0.5\"/>"
          << "<geom type=\"plane\" pos=\"0 0 -0.09\" size=\"1 1 0.1\" rgba=\"0.15 0.18 0.22 1\"/>";
        auto box = [&](std::string name, double r, double angle, double z, double a, double b, double h, std::string color) {
                x << "<geom name=\"" << name << "\" type=\"box\" pos=\"" << r * std::cos(angle) << ' ' << r * std::sin(angle)
                  << ' ' << z << "\" euler=\"0 0 " << angle << "\" size=\"" << a << ' ' << b << ' ' << h << "\" rgba=\""
                  << color << "\"/>";
        };
        auto cylinder = [&](std::string name, double r, double h, double z, std::string color) {
                x << "<geom name=\"" << name << "\" type=\"cylinder\" pos=\"0 0 " << z << "\" size=\"" << r << ' ' << h
                  << "\" rgba=\"" << color << "\"/>";
        };
        // Cutaway stator: every tooth and winding is visible; magnet count is exactly 2p
        for (int i = 0; i < c.slots; ++i) {
                double a = 2 * pi * i / c.slots;
                box("stator_tooth_" + std::to_string(i), .051, a, 0, .009, .038 / c.slots, .020, ".5 .55 .6 1");
                box("winding_" + std::to_string(i), .055, a, 0, .006, .09 / c.slots, .016, ".85 .38 .12 1");
                box("stator_yoke_" + std::to_string(i), .065, a, 0, .003, .205 / c.slots, .020, ".35 .4 .45 1");
        }
        x << "<body name=\"ava_rotor\" mocap=\"true\">";
        cylinder("rotor_core", .039, .020, 0, ".28 .3 .34 1");
        for (int i = 0; i < 2 * c.polePairs; ++i)
                box("magnet_" + std::to_string(i),
                    .041,
                    pi * i / c.polePairs,
                    0,
                    .002,
                    .048 / c.polePairs,
                    .020,
                    i % 2 ? ".15 .45 .95 1" : ".95 .2 .15 1");
        x << "</body>";
        auto gear = [&](std::string prefix, int teeth, double module, std::string color) {
                double r = module * teeth / 2;
                cylinder(prefix + "_core", r - module * .65, .008, 0, color);
                for (int t = 0; t < teeth; ++t)
                        box(prefix + "_tooth_" + std::to_string(t),
                            r,
                            2 * pi * t / teeth,
                            0,
                            module * .8,
                            module * .65,
                            .008,
                            color);
        };
        for (size_t i = 0; i < result.stages.size(); ++i) {
                auto        s      = result.stages[i];
                double      z      = .065 + i * (c.exploded ? .065 : .025);
                double      module = .12 / s.ring, radius = module * (s.sun + s.planet) / 2;
                std::string pre = "ava_stage_" + std::to_string(i);
                for (int t = 0; t < s.ring; ++t) {
                        box(pre + "_ring_" + std::to_string(t),
                            .06,
                            2 * pi * t / s.ring,
                            z,
                            module * .8,
                            module * .65,
                            .010,
                            ".35 .4 .46 1");
                        box(pre + "_rim_" + std::to_string(t),
                            .06 + module * 1.6,
                            2 * pi * t / s.ring,
                            z,
                            module * .8,
                            (.06 + module * 1.6) * pi / s.ring,
                            .010,
                            ".3 .35 .4 1");
                }
                x << "<body name=\"" << pre << "_sun\" mocap=\"true\" pos=\"0 0 " << z << "\">";
                gear(pre + "_sun", s.sun, module, ".95 .65 .15 1");
                x << "</body>";
                x << "<body name=\"" << pre << "_carrier\" mocap=\"true\" pos=\"0 0 " << z + .016 << "\">";
                for (int p = 0; p < 3; ++p)
                        box(pre + "_arm_" + std::to_string(p),
                            radius / 2,
                            2 * pi * p / 3,
                            0,
                            radius / 2,
                            .003,
                            .002,
                            ".2 .6 .8 1");
                x << "</body>";
                for (int p = 0; p < 3; ++p) {
                        double a = 2 * pi * p / 3;
                        x << "<body name=\"" << pre << "_planet_" << p << "\" mocap=\"true\" pos=\"" << radius * std::cos(a)
                          << ' ' << radius * std::sin(a) << ' ' << z << "\">";
                        gear(pre + "_planet_" + std::to_string(p), s.planet, module, ".6 .7 .8 1");
                        x << "</body>";
                }
        }
        x << "<body name=\"output\" pos=\"0 0 " << outputZ
          << "\"><joint name=\"output_joint\" type=\"hinge\" axis=\"0 0 1\" damping=\"0.03\" armature=\""
          << c.rotorInertia * result.ratio * result.ratio << "\"/><inertial pos=\"0 0 0\" mass=\"0.5\" diaginertia=\""
          << c.loadInertia << ' ' << c.loadInertia << ' ' << c.loadInertia << "\"/>";
        cylinder("output_flange", .048, .007, 0, ".25 .65 .45 1");
        box("output_marker", .03, 0, .008, .018, .002, .001, "1 1 1 1");
        x << "</body><body name=\"ava_encoder_disk\" mocap=\"true\" pos=\"0 0 -0.035\">";
        cylinder("encoder_magnet", .018, .002, 0, ".6 .2 .7 1");
        box("encoder_marker", .009, 0, .0021, .008, .001, .0003, "1 1 1 1");
        x << "</body>";
        x << "<body name=\"encoder_board\" pos=\"0 0 " << -.0396 - (c.gapMm + c.axialMm) * .001 << "\" euler=\""
          << c.tiltDeg * pi / 180 << " 0 0\">";
        box("encoder_pcb", 0, 0, 0, .027, .027, .001, ".1 .4 .18 1");
        box("encoder_sensor", 0, 0, .0018, .004, .004, .0008, ".15 .15 .17 1");
        x << "</body></worldbody><actuator><motor name=\"pmsm\" joint=\"output_joint\" gear=\"" << result.ratio * c.efficiency
          << "\" ctrllimited=\"true\" ctrlrange=\"" << -c.motorTorque << ' ' << c.motorTorque << "\"/></actuator></mujoco>";
        result.xml = x.str();
        return result;
}
} // namespace ava_actuator
