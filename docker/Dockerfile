# Shelf3DS build image: devkitARM + libctru/citro2d + makerom + bannertool
FROM devkitpro/devkitarm:latest

RUN apt-get update && apt-get install -y --no-install-recommends build-essential git ca-certificates zip \
    && rm -rf /var/lib/apt/lists/*

# makerom (Project_CTR)
RUN git clone --depth 1 https://github.com/3DSGuy/Project_CTR.git /tmp/ctr \
    && make -C /tmp/ctr/makerom deps && make -C /tmp/ctr/makerom program \
    && find /tmp/ctr/makerom -name makerom -type f -perm -u+x -exec cp {} /usr/local/bin/makerom \; \
    && rm -rf /tmp/ctr

# bannertool
RUN git clone --depth 1 --recursive https://github.com/diasurgical/bannertool.git /tmp/bt \
    && cd /tmp/bt && gcc -O2 -w -c source/pc/stb_image.c source/pc/stb_vorbis.c \
    && g++ -O2 -w -std=gnu++11 -DVERSION_MAJOR=1 -DVERSION_MINOR=2 -DVERSION_MICRO=2 -Isource source/*.cpp source/3ds/*.cpp source/pc/wav.cpp stb_image.o stb_vorbis.o -o /usr/local/bin/bannertool \
    && cd / \
    && rm -rf /tmp/bt

ENV DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
ENV PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitARM/bin:$PATH
WORKDIR /src
