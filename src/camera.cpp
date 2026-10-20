#include "camera.hpp"

#include <algorithm>
#include <cmath>

namespace phantom_fx::camera
{
namespace
{
constexpr int ProjectionView = 0, Projection = 4, ViewMatrix = 8, EyePosition = 20, ViewportSize = 22, CameraCenterOffset = 28;

// Gameplay never comes close in one frame at 60 fps: 2 m is 120 m/s, 25 degrees is 1500 degrees/s.
constexpr float CutDistance = 2.0f, CutCosine = 0.906f, CutFocalChange = 0.1f;

// Equal up to float rounding, relative to the size of the row the values come from.
bool near(float actual, float expected, float scale)
{
    return std::abs(actual - expected) <= 1e-4f * scale;
}

float largest(const float (&row)[4])
{
    return std::max({std::abs(row[0]), std::abs(row[1]), std::abs(row[2]), std::abs(row[3])});
}

bool rowIs(const float (&row)[4], float x, float y, float z, float w)
{
    return row[0] == x && row[1] == y && row[2] == z && row[3] == w;
}

// c0-c3 = P x X for an affine X: row 3 is X's z row and row 2 is A times it plus B, from P's
// third row (0, 0, A, B). Passes keep other matrices in c0-c3 only where this fails.
bool projectsWithProjection(const SceneConstants& scene)
{
    const float a = scene.c[Projection + 2][2], b = scene.c[Projection + 2][3];
    const auto& depthRow = scene.c[ProjectionView + 2];
    const auto& wRow = scene.c[ProjectionView + 3];
    const float scale = std::max(std::abs(a) * largest(wRow), std::abs(b));
    return scale > 0 && near(depthRow[0], a * wRow[0], scale) && near(depthRow[1], a * wRow[1], scale)
        && near(depthRow[2], a * wRow[2], scale) && near(depthRow[3], a * wRow[3] + b, scale);
}

double halton(std::uint64_t index, unsigned base)
{
    double result = 0, fraction = 1;
    for (; index; index /= base)
    {
        fraction /= base;
        result += fraction * static_cast<double>(index % base);
    }

    return result;
}

Matrix multiply(const Matrix& left, const Matrix& right)
{
    Matrix result{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            for (int k = 0; k < 4; ++k)
                result[row * 4 + column] += left[row * 4 + k] * right[k * 4 + column];

    return result;
}

// Gauss-Jordan with partial pivoting; a projection-view is always invertible.
Matrix inverse(Matrix matrix)
{
    Matrix result{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (int column = 0; column < 4; ++column)
    {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row)
            if (std::abs(matrix[row * 4 + column]) > std::abs(matrix[pivot * 4 + column])) pivot = row;

        for (int k = 0; k < 4; ++k)
        {
            std::swap(matrix[column * 4 + k], matrix[pivot * 4 + k]);
            std::swap(result[column * 4 + k], result[pivot * 4 + k]);
        }

        const double scale = 1 / matrix[column * 4 + column];
        for (int k = 0; k < 4; ++k)
        {
            matrix[column * 4 + k] *= scale;
            result[column * 4 + k] *= scale;
        }

        for (int row = 0; row < 4; ++row)
        {
            if (row == column) continue;

            const double factor = matrix[row * 4 + column];
            for (int k = 0; k < 4; ++k)
            {
                matrix[row * 4 + k] -= factor * matrix[column * 4 + k];
                result[row * 4 + k] -= factor * result[column * 4 + k];
            }
        }
    }

    return result;
}

Matrix transpose(const Matrix& matrix)
{
    Matrix result{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) result[column * 4 + row] = matrix[row * 4 + column];

    return result;
}

std::array<float, 16> toFloat(const Matrix& matrix)
{
    std::array<float, 16> result{};
    for (int i = 0; i < 16; ++i) result[i] = static_cast<float>(matrix[i]);

    return result;
}

// Row `row` of the view's rotation, the direction of one of the camera's axes in the world.
std::array<float, 3> axis(const Matrix& view, int row, double sign)
{
    const double x = view[row * 4], y = view[row * 4 + 1], z = view[row * 4 + 2];
    const double length = std::sqrt(x * x + y * y + z * z);
    if (length == 0) return {};

    return {static_cast<float>(sign * x / length), static_cast<float>(sign * y / length), static_cast<float>(sign * z / length)};
}
}

SceneView classify(const SceneConstants& scene)
{
    const auto& x = scene.c[Projection];
    const auto& y = scene.c[Projection + 1];
    const auto& z = scene.c[Projection + 2];
    const float width = scene.c[ViewportSize][0], height = scene.c[ViewportSize][1];
    // Shadow views are orthographic (c7 = 0, 0, 0, 1). A jittered or off-centre projection already
    // has a third column, and its offset in c28.
    const bool perspective = rowIs(scene.c[Projection + 3], 0, 0, 1, 0) && x[0] != 0 && x[1] == 0 && x[2] == 0 && x[3] == 0
        && y[0] == 0 && y[1] != 0 && y[2] == 0 && y[3] == 0 && z[0] == 0 && z[1] == 0;
    if (!perspective || !(width >= 1 && height >= 1) || scene.c[CameraCenterOffset][0] != 0 || scene.c[CameraCenterOffset][1] != 0)
        return SceneView::Other;

    // The main camera fills the viewport; a square spot-light shadow would not.
    if (std::abs(std::abs(y[1] / x[0]) / (width / height) - 1) > 0.01f) return SceneView::Other;

    // Rows 0, 1 and 3 of P x V are P00 V0, P11 V1 and V2.
    bool camera = projectsWithProjection(scene) && rowIs(scene.c[ViewMatrix + 3], 0, 0, 0, 1);
    for (int k = 0; camera && k < 4; ++k)
        camera = near(scene.c[ProjectionView][k], x[0] * scene.c[ViewMatrix][k], std::abs(x[0]) * largest(scene.c[ViewMatrix]))
            && near(scene.c[ProjectionView + 1][k], y[1] * scene.c[ViewMatrix + 1][k], std::abs(y[1]) * largest(scene.c[ViewMatrix + 1]))
            && near(scene.c[ProjectionView + 3][k], scene.c[ViewMatrix + 2][k], largest(scene.c[ViewMatrix + 2]));

    return camera ? SceneView::MainCamera : SceneView::MainProjection;
}

View viewOf(const SceneConstants& scene)
{
    View result;
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
        {
            result.projectionView[row * 4 + column] = scene.c[ProjectionView + row][column];
            result.projection[row * 4 + column] = scene.c[Projection + row][column];
            result.view[row * 4 + column] = scene.c[ViewMatrix + row][column];
        }

    const auto& forward = scene.c[ViewMatrix + 2];
    const float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    for (int i = 0; i < 3; ++i)
    {
        result.eye[i] = scene.c[EyePosition][i];
        result.forward[i] = length > 0 ? forward[i] / length : 0;
    }

    result.focal = scene.c[Projection + 1][1];
    result.width = scene.c[ViewportSize][0];
    result.height = scene.c[ViewportSize][1];
    result.nearPlane = scene.c[ViewportSize][2];
    result.farPlane = scene.c[ViewportSize][3];

    return result;
}

void applyJitter(SceneConstants& scene, std::array<float, 2> ndc)
{
    // NDC x = clip x / w, and the w row of a perspective P x X is X's z row (c3 here, c7 for P).
    if (projectsWithProjection(scene))
        for (int k = 0; k < 4; ++k)
        {
            scene.c[ProjectionView][k] += ndc[0] * scene.c[ProjectionView + 3][k];
            scene.c[ProjectionView + 1][k] += ndc[1] * scene.c[ProjectionView + 3][k];
        }

    scene.c[Projection][2] += ndc[0];
    scene.c[Projection + 1][2] += ndc[1];
    scene.c[CameraCenterOffset][0] = ndc[0];
    scene.c[CameraCenterOffset][1] = ndc[1];
}

std::array<float, 2> jitterNdc(std::array<float, 2> pixels, float width, float height)
{
    return {2 * pixels[0] / width, -2 * pixels[1] / height};
}

std::array<float, 2> jitterPixels(std::uint64_t frame, int phases)
{
    const auto index = frame % static_cast<std::uint64_t>(std::max(phases, 1)) + 1;
    return {static_cast<float>(halton(index, 2) - 0.5), static_cast<float>(halton(index, 3) - 0.5)};
}

int jitterPhases(float renderScale)
{
    if (!std::isfinite(renderScale) || renderScale <= 0) return 8;

    return std::clamp(static_cast<int>(std::ceil(8.0f / (renderScale * renderScale))), 8, 128);
}

std::array<float, 2> lensShift(const SceneConstants& scene, const std::array<float, 3>& centre, float focusDistance)
{
    if (!(focusDistance > 0)) return {};

    // The camera's offset in view space; its rotation stays, so view x and y move by this much.
    const float offset[3]{scene.c[EyePosition][0] - centre[0], scene.c[EyePosition][1] - centre[1], scene.c[EyePosition][2] - centre[2]};
    const auto along = [&offset](const float (&row)[4]) { return row[0] * offset[0] + row[1] * offset[1] + row[2] * offset[2]; };

    // NDC x = (P00 x + shift z) / z, so at z = focus the shift puts back what the offset took away.
    return {scene.c[Projection][0] * along(scene.c[ViewMatrix]) / focusDistance,
        scene.c[Projection + 1][1] * along(scene.c[ViewMatrix + 1]) / focusDistance};
}

std::array<float, 2> aperturePoint(std::uint32_t index, int blades)
{
    // Roberts' R2 sequence: any run of it covers the square evenly.
    constexpr double G = 1.32471795724474602596;
    const double u = std::fmod(0.5 + index / G, 1.0), v = std::fmod(0.5 + index / (G * G), 1.0);
    constexpr double Pi = 3.14159265358979323846;
    const double radius = std::sqrt(v);
    if (blades < 3)
    {
        const double angle = 2 * Pi * u;
        return {static_cast<float>(radius * std::cos(angle)), static_cast<float>(radius * std::sin(angle))};
    }

    // The triangle from the centre to two neighbouring corners, chosen by u, filled evenly.
    const double sector = 2 * Pi / blades, position = u * blades;
    const double corner = std::floor(position), along = position - corner;
    const double first = Pi / 2 + corner * sector, second = first + sector;
    return {static_cast<float>(radius * ((1 - along) * std::cos(first) + along * std::cos(second))),
        static_cast<float>(radius * ((1 - along) * std::sin(first) + along * std::sin(second)))};
}

Matrix previousFromCurrent(const View& previous, const View& current)
{
    return multiply(previous.projectionView, inverse(current.projectionView));
}

bool cameraCut(const View& previous, const View& current)
{
    float distance = 0, cosine = 0;
    for (int i = 0; i < 3; ++i)
    {
        distance += (current.eye[i] - previous.eye[i]) * (current.eye[i] - previous.eye[i]);
        cosine += current.forward[i] * previous.forward[i];
    }

    return std::sqrt(distance) > CutDistance || cosine < CutCosine || previous.focal == 0
        || std::abs(current.focal / previous.focal - 1) > CutFocalChange;
}

StreamlineCamera streamlineCamera(const View& previous, const View& current)
{
    // P x V = (P x M) x (M x V) with the mirror M = diag(-1, 1, 1, 1); the engine applies M x X as
    // dot(row, v), Streamline as v x M, so each matrix is transposed.
    auto projection = current.projection;
    for (int row = 0; row < 4; ++row) projection[row * 4] = -projection[row * 4];

    const auto toPrevious = previousFromCurrent(previous, current);
    StreamlineCamera result;
    result.viewToClip = toFloat(transpose(projection));
    result.clipToView = toFloat(transpose(inverse(projection)));
    result.clipToPreviousClip = toFloat(transpose(toPrevious));
    result.previousClipToClip = toFloat(transpose(inverse(toPrevious)));
    result.position = current.eye;
    result.right = axis(current.view, 0, -1);
    result.up = axis(current.view, 1, 1);
    result.forward = axis(current.view, 2, 1);
    result.nearPlane = current.nearPlane;
    result.farPlane = current.farPlane;
    result.verticalFov = static_cast<float>(2 * std::atan(1 / current.projection[5]));
    result.aspectRatio = static_cast<float>(current.projection[5] / projection[0]);

    return result;
}
}
