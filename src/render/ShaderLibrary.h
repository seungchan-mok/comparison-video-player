#pragma once

#include <QByteArray>
#include <QOpenGLShaderProgram>
#include <QString>

#include <map>
#include <memory>

namespace scvp::render {

// Compiles shader variants lazily from the qrc sources: "#version 330 core" plus one #define
// line per flag in `defines` (space separated) are prepended. SCVP_SHADER_DIR, when set, is
// read instead of the resources so shaders can be edited without rebuilding.
class ShaderLibrary {
public:
    QOpenGLShaderProgram* program(const QString& vertex, const QString& fragment, const QByteArray& defines);
    QString lastError() const { return m_error; }
    void clear() { m_programs.clear(); }

private:
    static QByteArray load(const QString& name);
    std::map<QByteArray, std::unique_ptr<QOpenGLShaderProgram>> m_programs;
    QString m_error;
};

} // namespace scvp::render
