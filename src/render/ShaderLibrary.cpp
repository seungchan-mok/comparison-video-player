#include "render/ShaderLibrary.h"

#include <QDir>
#include <QFile>

namespace scvp::render {

QByteArray ShaderLibrary::load(const QString& name)
{
    const QByteArray dir = qgetenv("SCVP_SHADER_DIR");
    QFile f(dir.isEmpty() ? QStringLiteral(":/scvp/shaders/") + name : QDir(QString::fromLocal8Bit(dir)).filePath(name));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

QOpenGLShaderProgram* ShaderLibrary::program(const QString& vertex, const QString& fragment, const QByteArray& defines)
{
    const QByteArray key = vertex.toUtf8() + '|' + fragment.toUtf8() + '|' + defines;
    auto it = m_programs.find(key);
    if (it != m_programs.end())
        return it->second.get();

    QByteArray header = "#version 330 core\n";
    for (const QByteArray& d : defines.split(' '))
        if (!d.isEmpty())
            header += "#define " + d + "\n";
    const QByteArray vs = load(vertex);
    const QByteArray fs = load(fragment);
    auto prog = std::make_unique<QOpenGLShaderProgram>();
    if (vs.isEmpty() || fs.isEmpty() || !prog->addShaderFromSourceCode(QOpenGLShader::Vertex, header + vs) ||
        !prog->addShaderFromSourceCode(QOpenGLShader::Fragment, header + fs) || !prog->link()) {
        m_error = QStringLiteral("%1/%2 [%3]: %4").arg(vertex, fragment, QString::fromUtf8(defines), prog->log());
        return nullptr;
    }
    QOpenGLShaderProgram* raw = prog.get();
    m_programs.emplace(key, std::move(prog));
    return raw;
}

} // namespace scvp::render
