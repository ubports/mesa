#!/bin/bash

git config --global user.email "mesa@example.com"
git config --global user.name "Mesa CI"
git clone \
    --depth 1 \
    https://github.com/KhronosGroup/VK-GL-CTS.git \
    -b opengl-es-cts-3.2.6.1 \
    /VK-GL-CTS
pushd /VK-GL-CTS

# surfaceless links against libkms and such despite not using it.
sed -i '/gbm/d' targets/surfaceless/surfaceless.cmake
sed -i '/libkms/d' targets/surfaceless/surfaceless.cmake
sed -i '/libgbm/d' targets/surfaceless/surfaceless.cmake

# --insecure is due to SSL cert failures hitting sourceforge for zlib and
# libpng (sigh).  The archives get their checksums checked anyway, and git
# always goes through ssh or https.
python3 external/fetch_sources.py --insecure

mkdir -p /deqp

# Save the testlog stylesheets:
cp doc/testlog-stylesheet/testlog.{css,xsl} /deqp
popd

pushd /deqp
cmake -G Ninja \
      -DDEQP_TARGET=surfaceless               \
      -DCMAKE_BUILD_TYPE=Release              \
      $EXTRA_CMAKE_ARGS                       \
      /VK-GL-CTS
ninja

# Copy out the mustpass lists we want from a bunch of other junk.
mkdir /deqp/mustpass
for gles in gles2 gles3 gles31; do
    cp \
        /deqp/external/openglcts/modules/gl_cts/data/mustpass/gles/aosp_mustpass/3.2.6.x/$gles-master.txt \
        /deqp/mustpass/$gles-master.txt
done
cp \
    /deqp/external/openglcts/modules/gl_cts/data/mustpass/gl/khronos_mustpass/4.6.1.x/*-master.txt \
    /deqp/mustpass/.



# Save *some* executor utils, but otherwise strip things down
# to reduct deqp build size:
mkdir /deqp/executor.save
cp /deqp/executor/testlog-to-* /deqp/executor.save
rm -rf /deqp/executor
mv /deqp/executor.save /deqp/executor

ls /deqp/external | grep -v openglcts | xargs rm -rf
rm -rf /deqp/modules/internal
rm -rf /deqp/execserver
rm -rf /deqp/modules/egl
rm -rf /deqp/framework
rm -rf /deqp/external/openglcts/modules/gl_cts/data/mustpass
rm -rf /deqp/external/openglcts/modules/cts-runner
rm -rf /deqp/external/vulkancts/modules/vulkan/vk-build-programs
find -iname '*cmake*' -o -name '*ninja*' -o -name '*.o' -o -name '*.a' | xargs rm -rf
${STRIP_CMD:-strip} modules/*/deqp-* external/openglcts/modules/glcts
du -sh *
rm -rf /VK-GL-CTS
popd
