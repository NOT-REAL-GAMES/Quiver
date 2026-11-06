/* Keep the engine's C interface independent of legacy tool headers. */
namespace quiver_qbsp { int qbsp_main(int, const char **); }
namespace quiver_vis { int vis_main(int, const char **); }
namespace quiver_light { int light_main(int, const char **); }
extern "C" int Quiver_QBSPMain(int argc, const char **argv) { return quiver_qbsp::qbsp_main(argc, argv); }
extern "C" int Quiver_VISMain(int argc, const char **argv) { return quiver_vis::vis_main(argc, argv); }
extern "C" int Quiver_LIGHTMain(int argc, const char **argv) { return quiver_light::light_main(argc, argv); }
