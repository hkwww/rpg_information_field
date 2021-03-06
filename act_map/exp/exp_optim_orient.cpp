#include "act_map/optim_orient.h"

#include <random>

#include <rpg_common/main.h>
#include <rpg_common/timer.h>
#include <rpg_common/save.h>
#include <rpg_common/fs.h>
#include <vi_utils/map.h>
#include <vi_utils/cam_min.h>
#include <vi_utils/common_utils.h>

#include "act_map/sampler.h"
#include "act_map/conversion.h"

DEFINE_string(abs_map, "", "absolute path of the map file.");
DEFINE_string(abs_trace_dir, "", "trace dir to save results");

DEFINE_double(xrange, 5.0, "X range centered at zero.");
DEFINE_double(yrange, 5.0, "Y range centered at zero.");
DEFINE_double(zrange, 2.0, "Z range centered at zero.");
DEFINE_double(vox_res, 0.5, "Voxel size.");

DEFINE_double(angle_res_deg, 10.0, "resolution to sample points on a sphere.");
DEFINE_double(check_ratio, 0.05, "ratio of voxels to compute.");

using namespace act_map;
using namespace act_map::optim_orient;

RPG_COMMON_MAIN
{
  CHECK(!FLAGS_abs_map.empty());
  CHECK(!FLAGS_abs_trace_dir.empty());

  std::cout << "This is to test whether the map can be used to select motion. "
               "In this case, we select the optimal orientation at each point. "
               "Samples on SO3 are checked to find the optimal orientation."
               "\n";

  std::cout << "Experiment parameters:\n"
            << "- abs_map: " << FLAGS_abs_map << std::endl
            << "- abs_trace_dir: " << FLAGS_abs_trace_dir << std::endl
            << "- xrange: " << FLAGS_xrange << std::endl
            << "- yrange: " << FLAGS_yrange << std::endl
            << "- zrange: " << FLAGS_zrange << std::endl
            << "- vox_res: " << FLAGS_vox_res << std::endl
            << "- angel_res_deg: " << FLAGS_angle_res_deg << std::endl
            << "- check_ratio: " << FLAGS_check_ratio << std::endl;

  std::srand(std::time(nullptr));
  std::random_device rd;
  std::mt19937 rng(rd());

  rpg::Pose Tbc;
  Tbc.setIdentity();
  vi::PinholeCam cam({ 300, 300, 300, 300, 600, 600 }, Tbc);
  VLOG(1) << "Simulated a camera:\n" << cam;

  double hfov_rad = M_PI_4;
  QuadraticVisScore vscore(hfov_rad);
  vscore.initSecondOrderApprox(0.8, 0.8);

  std::string dir;
  std::string fn;
  rpg::fs::splitPathAndFilename(__FILE__, &dir, &fn);
  std::string gp_vis_profile = dir + "/../params/fov_approximator_gp/"
//                                     "fov50_fs50_lm1000_k10";
//                                     "fov45_fs30_lm1000_k10";
//                                     "fov45_fs50_lm1000_k10_fast";
                                     "fov45_fs70_lm1000_k15";
  VLOG(1) << "Loading GP visibility profile at " << gp_vis_profile;
  VisApproxPtr<GPVisApprox> gp_vis_ptr =
      std::make_shared<GPVisibilityApproximator>();
  gp_vis_ptr->load(gp_vis_profile);
  GPInfoVoxel::setVisApprox(gp_vis_ptr);
  GPTraceVoxel::setVisApprox(gp_vis_ptr);
  VLOG(1) << *gp_vis_ptr;

  rpg::Timer timer;

  vi_utils::Map map;
  map.load(FLAGS_abs_map, std::string());
  VLOG(1) << "Loaded " << map.n_points_ << " map points.";

  std::vector<double> xvalues, yvalues, zvalues;
  rpg::PositionVec uniform_grid;
  utils::generateUniformPointsWithin(FLAGS_vox_res, FLAGS_xrange, FLAGS_yrange,
                                     FLAGS_zrange, &uniform_grid);
  const size_t kNPts = uniform_grid.size();
  const size_t kNCompute = static_cast<size_t>(FLAGS_check_ratio * kNPts);
  std::uniform_int_distribution<size_t> pts_uni(0, kNPts - 1);
  VLOG(1) << "Random sampling voxel positions...";
  rpg::PositionVec vox_pos;
  for (size_t i = 0; i < kNCompute; i++)
  {
    vox_pos.emplace_back(uniform_grid[pts_uni(rng)]);
  }

  VLOG(1) << "Computing the kernels for " << kNCompute << " random voxels...";
  // quadratic kernels
  InfoK1Vec info_k1_vec(kNCompute);
  InfoK2Vec info_k2_vec(kNCompute);
  InfoK3Vec info_k3_vec(kNCompute);
  TraceK1Vec trace_k1_vec(kNCompute);
  TraceK2Vec trace_k2_vec(kNCompute);
  TraceK3Vec trace_k3_vec(kNCompute);
  // GP voxels
  GPInfoVoxelVec gp_info_vec(kNCompute);
  GPTraceVoxelVec gp_trace_vec(kNCompute);

  double cnt_r = 0.0;
  double tbuild_info = 0;
  double tbuild_trace = 0;
  double tbuild_gp_info = 0;
  double tbuild_gp_trace = 0;
  for (size_t i = 0; i < kNCompute; i++)
  {
    if (i > cnt_r * kNCompute)
    {
      std::cout << "." << std::flush;
      cnt_r += 0.1;
    }
    timer.start();
    constructFactorBatch(map.points_, vox_pos[i], &(info_k1_vec[i]),
                         &(info_k2_vec[i]), &(info_k3_vec[i]));
    tbuild_info += timer.stop();
    timer.start();
    constructFactorBatch(map.points_, vox_pos[i], &(trace_k1_vec[i]),
                         &(trace_k2_vec[i]), &(trace_k3_vec[i]));
    tbuild_trace += timer.stop();
    timer.start();
    constructFactorVoxelBatch(map.points_, vox_pos[i], &(gp_info_vec[i]));
    tbuild_gp_info += timer.stop();

    timer.start();
    constructFactorVoxelBatch(map.points_, vox_pos[i], &(gp_trace_vec[i]));
    tbuild_gp_trace += timer.stop();
  }
  VLOG(1) << "Done computing the kernels.";

  VLOG(1) << "Sampling rotations...";

  rpg::RotationVec rot_samples;
  utils::sampleRotation(FLAGS_angle_res_deg, &rot_samples);

  std::vector<InfoMetricType> test_types{ InfoMetricType::kMinEig,
                                          InfoMetricType::kDet,
                                          InfoMetricType::kTrace };
  std::map<InfoMetricType, OptimOrientRes> res_appr;
  std::map<InfoMetricType, OptimOrientRes> res_gp_appr;
  std::map<InfoMetricType, OptimOrientRes> res_exact;
  // additional tests
  OptimOrientRes res_appr_trace_closed(kNCompute, "trace_appr_zero_deriv");
  OptimOrientRes res_appr_trace_worst(kNCompute, "trace_appr_worst");

  for (const InfoMetricType v : test_types)
  {
    const std::string nm = kInfoMetricNames[v];
    res_appr.insert({ v, OptimOrientRes(kNCompute, nm + "_app") });
    res_gp_appr.insert({ v, OptimOrientRes(kNCompute, nm + "_gp_app") });
    res_exact.insert({ v, OptimOrientRes(kNCompute, nm + "_exact") });
  }

  VLOG(1) << ">>> Determine the optimal orientation from the approximated"
             " information.";
  cnt_r = 0.0;
  std::map<InfoMetricType, double> quad_appr_times{
    { InfoMetricType::kMinEig, 0 },
    { InfoMetricType::kDet, 0 },
    { InfoMetricType::kTrace, 0 }
  };
  std::map<InfoMetricType, double> gp_appr_times(quad_appr_times);
  double tquery_gp_trace = 0;

  double tquery_trace_closed = 0;
  double tquery_trace_worst = 0;
  for (size_t i = 0; i < kNCompute; i++)
  {
    if (i > cnt_r * kNCompute)
    {
      std::cout << "." << std::flush;
      cnt_r += 0.1;
    }

    // quadratic kernels
    for (const InfoMetricType v : test_types)
    {
      timer.start();
      optim_orient::getOptimViewFromInfoKernels(
          rot_samples, vscore.k1(), vscore.k2(), vscore.k3(), info_k1_vec[i],
          info_k2_vec[i], info_k3_vec[i], v, &(res_appr[v].optim_views_[i]),
          &(res_appr[v].optim_vals_[static_cast<Eigen::Index>(i)]));
      quad_appr_times[v] += timer.stop();
    }
    // GP kernels
    for (const InfoMetricType v : test_types)
    {
      timer.start();
      optim_orient::getOptimViewFromGPInfoVoxel(
          rot_samples, gp_info_vec[i], v, &(res_gp_appr[v].optim_views_[i]),
          &(res_gp_appr[v].optim_vals_[static_cast<Eigen::Index>(i)]));
      gp_appr_times[v] += timer.stop();
    }
    timer.start();
    optim_orient::getOptimViewFromGPTraceVoxel(
        rot_samples, gp_trace_vec[i],
        &(res_gp_appr[InfoMetricType::kTrace].optim_views_[i]),
        &(res_gp_appr[InfoMetricType::kTrace]
              .optim_vals_[static_cast<Eigen::Index>(i)]));
    tquery_gp_trace += timer.stop();

    // extra tests
    timer.start();
    optim_orient::getMinTraceDirectionFromTraceKernelsUnconstrained(
        vscore.k1(), vscore.k2(), vscore.k3(), trace_k1_vec[i], trace_k2_vec[i],
        trace_k3_vec[i], &(res_appr_trace_closed.optim_views_[i]),
        &(res_appr_trace_closed.optim_vals_[static_cast<Eigen::Index>(i)]));
    tquery_trace_closed += timer.stop();

    timer.start();
    optim_orient::getWorstViewFromTraceKernels(
        rot_samples, vscore.k1(), vscore.k2(), vscore.k3(), trace_k1_vec[i],
        trace_k2_vec[i], trace_k3_vec[i],
        &(res_appr_trace_worst.optim_views_[i]),
        &(res_appr_trace_worst.optim_vals_[static_cast<Eigen::Index>(i)]));
    tquery_trace_worst += timer.stop();
  }
  VLOG(1) << "Timing (sec):\n"
          << "Build: Info - " << tbuild_info << ", Trace - " << tbuild_trace
          << ", GP-Info - " << tbuild_gp_info
          << ". GP-Trace - " << tbuild_gp_trace << std::endl;
  VLOG(1) << "Query - Quadratic:";
  for (const InfoMetricType v : test_types)
  {
    VLOG(1) << "- " << kInfoMetricNames[v] << ": " << quad_appr_times[v];
  }
  VLOG(1) << "Query - GP:";
  for (const InfoMetricType v : test_types)
  {
    VLOG(1) << "- " << kInfoMetricNames[v] << ": " << gp_appr_times[v];
  }
  VLOG(1) << "- " << "GP trace voxel "<< ": " <<  tquery_gp_trace;

  VLOG(1) << ">>> Determine the optimal orientation from exact "
             "information...";
  std::map<InfoMetricType, double> time_exact;
  for (const InfoMetricType v : test_types)
  {
    time_exact.insert({ v, 0 });
  }
  cnt_r = 0.0;
  for (size_t i = 0; i < kNCompute; i++)
  {
    if (i > cnt_r * kNCompute)
    {
      std::cout << "." << std::flush;
      cnt_r += 0.1;
    }
    const Eigen::Vector3d& twc_i = vox_pos[i];

    for (const InfoMetricType v : test_types)
    {
      timer.start();
      optim_orient::getOptimalViewFromExactInfo(
          rot_samples, twc_i, map.points_, cam, v,
          &(res_exact[v].optim_views_[i]), &(res_exact[v].optim_vals_[i]));
      time_exact[v] += timer.stop();
    }
  }
  VLOG(1) << "Timing (sec):\n"
          << "MinEig: " << time_exact[InfoMetricType::kMinEig] << ", Trace - "
          << time_exact[InfoMetricType::kTrace] << ", Det - "
          << time_exact[InfoMetricType::kDet];

  VLOG(1) << "Saving results...";
  Eigen::MatrixX3d vox_pos_mat;
  VecKVecToEigenXK(vox_pos, &vox_pos_mat);
  rpg::save(FLAGS_abs_trace_dir + "/vox_pos.txt", vox_pos_mat);
  for (const InfoMetricType mtype : test_types)
  {
    res_appr[mtype].save(FLAGS_abs_trace_dir);
    res_gp_appr[mtype].save(FLAGS_abs_trace_dir);
    res_exact[mtype].save(FLAGS_abs_trace_dir);
  }
  res_appr_trace_closed.save(FLAGS_abs_trace_dir);
  res_appr_trace_worst.save(FLAGS_abs_trace_dir);
}
