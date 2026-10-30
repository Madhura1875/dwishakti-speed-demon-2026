FROM gcc:15@sha256:eb5e64286ad47187b32bda75c6b6916166ecb41da56559b8b09e0001748d2cce

WORKDIR /app

COPY . .

RUN g++ -O3 -std=c++20 -o /app/solution main.cpp

ENTRYPOINT ["/app/solution"]