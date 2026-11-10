// SPDX-License-Identifier: GPL-2.0-or-later
// Actual effect fragment shader in an HDR float framebuffer. Readback is test-only.
#include <core/colorspace.h>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDirIterator>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <array>

using namespace KWin;
static void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static QByteArray resource(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(("Missing resource " + path).toStdString());
    return file.readAll();
}

class Probe : public QOpenGLFunctions_3_3_Core
{
public:
    QOpenGLContext context;
    QOffscreenSurface surface;
    QOpenGLShaderProgram program;
    GLuint textures[3]{}, fbo = 0, vao = 0;
    std::shared_ptr<ColorDescription> hdr = ColorDescription::BT2020PQ->withHdrMetadata(650, 650);
    bool sdrShader;
    Probe(bool sdr = false) : sdrShader(sdr)
    {
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        context.setFormat(format);
        require(context.create(), "GL context creation failed");
        surface.setFormat(context.format()); surface.create();
        require(context.makeCurrent(&surface) && initializeOpenGLFunctions(), "GL context activation failed");
        QByteArray fragment = resource(sdrShader ? ":/kwin-vfx/enhance.frag" : ":/kwin-vfx/enhance-hdr.frag");
        if (sdrShader) {
            fragment.replace("#include \"kwin-vfx/filter.glsl\"",resource(":/opengl/kwin-vfx/filter.glsl"));
            fragment.replace("#include \"kwin-vfx/cas.glsl\"",resource(":/opengl/kwin-vfx/cas.glsl"));
        }
        // Qt's shader wrapper detects #version only at the start (KWin's own
        // loader handles the preceding SPDX comment itself).
        fragment = fragment.mid(fragment.indexOf("#version"));
        // Resolve the exact KWin resources linked from the installed matching library.
        for (const auto &name : {"colormanagement.glsl", "saturation.glsl"}) {
            QString path;
            QDirIterator it(":/", QDirIterator::Subdirectories);
            while (it.hasNext()) { const auto next = it.next(); if (next.endsWith('/' + QString(name))) { path = next; break; } }
            require(!path.isEmpty(), "Missing installed KWin GLSL resource");
            fragment.replace(QByteArray("#include \"") + name + '"', resource(path));
        }
        require(program.addShaderFromSourceCode(QOpenGLShader::Vertex, R"(#version 140
out vec2 texcoord0;
void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); texcoord0 = p; gl_Position = vec4(p*2.0-1.0,0,1); }
)"), "Vertex compile failed");
        if (!program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragment))
            throw std::runtime_error(("HDR shader compile failed: " + program.log()).toStdString());
        require(program.link() && program.bind(), "HDR shader link failed");
        glGenVertexArrays(1, &vao); glBindVertexArray(vao);
        glGenTextures(3, textures);
        for (int i = 0; i < 3; ++i) {
            glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_2D, textures[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, nullptr);
        }
        glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[2], 0);
        require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Float FBO unavailable");
        glViewport(0, 0, 1, 1); glDisable(GL_BLEND); glDisable(GL_DITHER);
        program.setUniformValue("sampler", 0); program.setUniformValue("aiSampler", 1);
        program.setUniformValue("modulation", QVector4D(1,1,1,1));
        program.setUniformValue("saturation", 1.0f);
        program.setUniformValue("primaryBrightness", QVector3D(0.2627f,0.6780f,0.0593f));
        program.setUniformValue("texelSize", QVector2D(1,1));
        const auto source = sdrShader ? ColorDescription::sRGB : hdr;
        program.setUniformValue("colorimetryTransform", source->toOther(*hdr, RenderingIntent::Perceptual));
        program.setUniformValue("sourceNamedTransferFunction", int(source->transferFunction().type));
        program.setUniformValue("destinationNamedTransferFunction", int(TransferFunction::PerceptualQuantizer));
        const auto sourceTf=source->transferFunction();
        program.setUniformValue("sourceTransferFunctionParams", QVector2D(sourceTf.minLuminance,sourceTf.maxLuminance-sourceTf.minLuminance));
        program.setUniformValue("destinationTransferFunctionParams", QVector2D(0,10000));
        program.setUniformValue("sourceReferenceLuminance", float(source->referenceLuminance()));
        program.setUniformValue("destinationReferenceLuminance", float(hdr->referenceLuminance()));
        program.setUniformValue("maxTonemappingLuminance", 650.0f);
        program.setUniformValue("maxDestinationLuminance", 650.0f);
        program.setUniformValue("destinationToLMS", hdr->containerColorimetry().toLMS());
        program.setUniformValue("lmsToDestination", hdr->containerColorimetry().fromLMS());
        program.setUniformValue("originalToDestination", ColorDescription::sRGB->toOther(*hdr, RenderingIntent::Perceptual));
        const auto tf = ColorDescription::sRGB->transferFunction();
        program.setUniformValue("originalTransferParams", QVector2D(tf.minLuminance, tf.maxLuminance-tf.minLuminance));
        if (sdrShader) {
            program.setUniformValue("aiTexelSize",QVector2D(1,1));
            program.setUniformValue("aiSharpenStrength",1.0f);
        }
    }
    ~Probe() { context.makeCurrent(&surface); glDeleteFramebuffers(1,&fbo); glDeleteTextures(3,textures); glDeleteVertexArrays(1,&vao); }
    std::array<float,4> render(float alpha, float strength, float nits, float grain=0, int frame=0)
    {
        const float pq = sdrShader ? 0.5f : hdr->transferFunction().whiteNitsToEncoded(nits);
        const float original[4]{0.5f*alpha,0.5f*alpha,0.5f*alpha,alpha}, ai[4]{pq,pq,pq,1};
        for (int i=0;i<2;++i) {
            glActiveTexture(GL_TEXTURE0+i); glBindTexture(GL_TEXTURE_2D,textures[i]);
            glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1,1,GL_RGBA,GL_FLOAT,i==0?original:ai);
        }
        program.setUniformValue("enhancementStrength",strength);
        program.setUniformValue("grainStrength",grain); program.setUniformValue("grainFrame",frame);
        glDrawArrays(GL_TRIANGLES,0,3);
        std::array<float,4> out;
        glReadPixels(0,0,1,1,GL_RGBA,GL_FLOAT,out.data());
        require(glGetError()==GL_NO_ERROR,"GL error in HDR shader probe");
        return out;
    }
};

int main(int argc,char **argv)
{
    QGuiApplication app(argc,argv);
    try {
        Probe p;
        const auto originalPq = ColorDescription::sRGB->mapTo(QVector3D(.5f,.5f,.5f),*p.hdr,RenderingIntent::Perceptual);
        const auto originalNits = p.hdr->transferFunction().encodedToNits(originalPq);
        for (float alpha : {1.0f,0.5f,0.02f}) for (float strength : {0.0f,0.5f,1.0f}) {
            const auto out = p.render(alpha,strength,200);
            const auto expectedNits = originalNits*(1-strength)+QVector3D(200,200,200)*strength;
            const auto expected = p.hdr->transferFunction().nitsToEncoded(expectedNits)*alpha;
            require(std::abs(out[3]-alpha)<0.00001f,"Alpha changed");
            for (int c=0;c<3;++c) require(std::abs(out[c]-expected[c])<0.0001f,"PQ blend differs from CPU color reference");
        }
        const auto peak=p.render(1,1,650);
        const auto peakNits=p.hdr->transferFunction().whiteEncodedToNits(peak[0]);
        require(std::abs(peakNits-650)<0.25,"HDR highlight clipped to SDR white");
        const auto transparent=p.render(0,1,650);
        for(float c:transparent) require(c==0,"Transparent pixel changed");
        const auto a=p.render(1,1,200,1,0),b=p.render(1,1,200,1,1);
        require(a[0]!=b[0] && a[3]==b[3],"HDR grain did not animate or changed alpha");
        require(p.render(1,1,200)==p.render(1,1,200),"Grain off is unstable");
        {
            Probe sdr(true);
            const auto expectedColor=ColorDescription::sRGB->mapTo(QVector3D(.5f,.5f,.5f),*sdr.hdr,RenderingIntent::Perceptual);
            for (int mode : {1,2,3}) for (float alpha : {1.0f,0.5f,0.02f,0.0f}) {
                sdr.program.setUniformValue("filterMode",mode);
                const auto out=sdr.render(alpha,1,200);
                require(std::abs(out[3]-alpha)<0.00001f,"SDR enhancement on HDR changed alpha");
                for (int c=0;c<3;++c) {
                    if (std::abs(out[c]-expectedColor[c]*alpha)>=0.0001f)
                        std::cerr << "mode=" << mode << " alpha=" << alpha << " channel=" << c << " output=" << out[c]
                                  << " expected=" << expectedColor[c]*alpha << '\n';
                    require(std::abs(out[c]-expectedColor[c]*alpha)<0.0001f,"SDR enhancement on HDR differs from CPU color reference");
                }
            }
        }
        const QJsonObject result{{"passed",true},{"actual_effect_shader",true},{"target","BT.2020 PQ RGBA32F"},
            {"highlight_nits",peakNits},{"linear_nits_blend",true},{"alpha_preserved",true},{"grain_animates",true},
            {"sdr_enhancement_on_hdr_without_truehdr",true}};
        const auto data=QJsonDocument(result).toJson(); std::cout<<data.constData();
        const int i=app.arguments().indexOf("--output");
        if(i>=0) { require(i+1<argc,"Missing output path"); QFile file(app.arguments()[i+1]); require(file.open(QIODevice::WriteOnly),"Cannot write result"); file.write(data); }
        return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
