#include "pipeline/ConversionJob.hpp"

#include "export/GfxFormatWriter.hpp"
#include "export/SceneModel.hpp"
#include "util/PathUtils.hpp"

#include <chrono>
#include <exception>
#include <sstream>

namespace fs = std::filesystem;

namespace gfnif {
namespace {

std::uintmax_t FileSizeOrZero(const fs::path& p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0u : n;
}

} // namespace

JobResult RunConversionJob(const ScannedModel& model, const JobOptions& options) {
    const auto start = std::chrono::steady_clock::now();

    JobResult r;
    r.nifPath = model.nifPath;
    r.hadCompanionKf = !model.kfPath.empty();
    r.outBase = OutputBaseFor(model.nifPath, options.inputRoot, options.outputRoot);

    auto finish = [&](JobStatus status) {
        r.status = status;
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return r;
    };

    auto debug = [&](const std::string& line) {
        if (options.collectDebugLines) {
            r.debugLines.push_back(line);
        }
    };

    // The whole body is guarded: niflib throws std::exception subclasses for
    // malformed files (and item/model/WF20.nif throws bad_alloc), and an
    // exception escaping a worker thread would call std::terminate and kill the
    // entire run rather than costing one file.
    try {
        if (!options.overwrite && OutputIsUpToDate(model.nifPath, r.outBase)) {
            debug("output newer than source, skipping (use --overwrite to force)");
            return finish(JobStatus::Skipped);
        }

        debug("loading " + ToPosix(model.nifPath));
        if (r.hadCompanionKf) {
            debug("companion animation: " + ToPosix(model.kfPath));
        }

        SceneData scene;
        r.extraction = ExtractScene(model.nifPath.string(), scene, options.verifyStrips);

        if (!r.extraction.success) {
            r.error = r.extraction.error;
            return finish(JobStatus::Failed);
        }

        r.meshCount = scene.meshes.size();
        r.materialCount = scene.materials.size();
        r.vertexCount = scene.TotalVertices();
        r.triangleCount = scene.TotalTriangles();
        r.skeletonCount = scene.skeletons.size();
        for (const SkeletonData& s : scene.skeletons) {
            r.boneCount += s.bones.size();
        }
        r.clipCount = scene.animations.size();
        r.particleSystemCount = scene.particleSystems.size();

        for (const MaterialData& m : scene.materials) {
            if (m.sourceTextureName.empty()) {
                continue;
            }
            if (m.textureFound) {
                ++r.texturesResolved;
            } else {
                ++r.texturesMissing;
                r.unresolvedTextures.push_back(model.nifPath.filename().string() + " -> " +
                                               m.sourceTextureName);
            }
        }

        {
            std::ostringstream os;
            os << "blocks -> " << r.meshCount << " mesh(es), " << r.materialCount
               << " material(s), " << r.vertexCount << " verts, " << r.triangleCount << " tris, "
               << r.boneCount << " bone(s), " << r.clipCount << " clip(s), "
               << r.particleSystemCount << " particle system(s)";
            debug(os.str());
        }

        if (options.dryRun) {
            debug("dry run: would write " + ToPosix(r.outBase) + ".gfmodel/.gfbin");
            return finish(JobStatus::DryRun);
        }

        std::string dirError;
        if (!EnsureParentDirectory(r.outBase, dirError)) {
            r.error = dirError;
            return finish(JobStatus::Failed);
        }

        std::string writeError;
        if (!WriteSceneFiles(scene, r.outBase.string(), writeError)) {
            r.error = writeError;
            return finish(JobStatus::Failed);
        }

        r.bytesWritten = FileSizeOrZero(fs::path(r.outBase).concat(".gfmodel")) +
                         FileSizeOrZero(fs::path(r.outBase).concat(".gfbin"));
        debug("wrote " + ToPosix(r.outBase) + ".gfmodel/.gfbin (" +
              std::to_string(r.bytesWritten) + " bytes)");

        return finish(JobStatus::Converted);
    } catch (const std::exception& e) {
        r.error = std::string("exception: ") + e.what();
        return finish(JobStatus::Failed);
    } catch (...) {
        r.error = "unknown exception";
        return finish(JobStatus::Failed);
    }
}

} // namespace gfnif
