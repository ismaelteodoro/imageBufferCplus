# imageBufferCplus
get image rpi imx296

--------------------------

## Estado atual

O projeto já possui a base C++/CMake do engine de aquisição e um executável funcional:

* abre a câmera IMX296 diretamente com libcamera;
* solicita stream RAW Bayer packed `SRGGB10_CSI2P`;
* valida resolução `1456x1088`;
* aceita o ajuste real do driver Raspberry Pi para outro Bayer RAW10 CSI-2 packed equivalente, observado como `SBGGR10_CSI2P`;
* desativa auto exposure e auto white balance;
* aplica exposição fixa, ganho fixo e frame duration fixo;
* captura continuamente;
* re-enfileira buffers libcamera sem alocação contínua;
* mapeia os buffers da libcamera uma vez com `mmap`;
* cria `FrameDescriptor` e `RawFrameView` para cada frame capturado;
* copia cada frame RAW para um ring buffer circular pré-alocado de 150 frames;
* executa servidor TCP para solicitar frames sob demanda;
* imprime `frame_id`, `sequence`, `timestamp_ns`, delta entre frames, payload, FPS, uso do ring buffer, overwrites e frames descartados.

O TCP usa snapshots do ring buffer antes de transmitir, evitando que o payload enviado seja sobrescrito enquanto um cliente lento ainda está recebendo dados.

Estrutura atual:

```text
.
├── CMakeLists.txt
├── example_capture_tcp/
├── include/image_buffer/CaptureEngine.hpp
├── include/image_buffer/FrameTypes.hpp
├── include/image_buffer/RingBuffer.hpp
├── include/image_buffer/TcpServer.hpp
├── main.cpp
├── src/CaptureEngine.cpp
├── src/RingBuffer.cpp
└── src/TcpServer.cpp
```

Build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Execução:

```bash
./build/image_buffer_capture
```

Opções úteis:

```bash
./build/image_buffer_capture --exposure-us 8000 --gain 1.0 --frame-us 16666 --ring-frames 150 --stats-every 60 --tcp-port 8000
```

Para testar somente captura/ring buffer sem TCP:

```bash
./build/image_buffer_capture --no-tcp
```

Protocolo TCP:

O cliente envia comandos texto terminados em `\n`:

```text
PING
GET_LATEST
GET_LAST N
GET_FRAME ID
```

Resposta de sucesso:

```text
OK <quantidade_de_frames>\n
```

Depois da linha `OK`, o servidor envia cada frame como:

```text
FrameHeader binário little-endian
payload RAW10 packed
```

Header binário por frame:

```c
struct FrameHeader {
    uint32_t magic;        // 0x314d5849
    uint64_t frame_id;
    uint64_t timestamp_ns;
    uint32_t width;
    uint32_t height;
    uint32_t payload_size;
};
```

Resposta de erro:

```text
ERR <mensagem>\n
```

Cliente de exemplo:

A pasta `example_capture_tcp` contém um cliente Python simples para rodar em outra máquina, requisitar frames via rede, desempacotar RAW10 e mostrar a imagem na tela. Ele serve como base para conectar uma etapa posterior de análise por IA.

```bash
cd example_capture_tcp
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python3 capture_client.py --host <IP_DO_RASPBERRY> --port 8000 --mode latest --count 20
```

Observação sobre formato:

O código solicita `SRGGB10_CSI2P`, mas neste ambiente a libcamera validou o stream como:

```text
1456x1088-SBGGR10_CSI2P
```

Isso mantém RAW10 CSI-2 packed, resolução cheia e payload cru. A diferença é o padrão Bayer reportado pelo pipeline.

--------------------------


Você é um engenheiro sênior especialista em C++, Linux embarcado, libcamera, machine vision e sistemas de aquisição de alta velocidade.

Seu objetivo é criar um projeto C++ moderno para Raspberry Pi 4 + câmera Sony IMX296 utilizando libcamera diretamente, sem Picamera2 e sem Python.

O sistema deve funcionar como um engine de aquisição RAW10 contínuo em RAM com ring buffer circular e trigger TCP sob demanda.

IMPORTANTE:
O foco principal NÃO é visualização.
O foco é:

* captura contínua estável
* mínima latência
* mínima cópia
* uso eficiente de RAM
* manter 60 FPS reais
* zero gravação em disco
* zero compressão
* pipeline industrial

========================
OBJETIVO GERAL
==============

Capturar continuamente frames RAW10 do sensor IMX296 a 60 FPS reais e manter os últimos 150 frames em RAM utilizando um ring buffer circular.

Clientes TCP poderão solicitar frames específicos sob demanda.

O Raspberry NÃO deve:

* fazer JPEG
* fazer OpenCV
* fazer debayer
* fazer RGB
* gravar disco
* comprimir nada

O Raspberry apenas:

* captura RAW10
* armazena em RAM
* envia bytes crus quando solicitado

Todo processamento ocorrerá no cliente remoto.

========================
HARDWARE
========

* Raspberry Pi 4 Model B
* câmera Sony IMX296 global shutter
* CSI-2
* Linux 64-bit
* libcamera instalado

========================
REQUISITOS DE CAPTURA
=====================

Modo:

* RAW10 Bayer packed

Resolução:

* máxima resolução do sensor
* 1456x1088

FPS:

* 60 FPS reais contínuos

Desativar:

* auto exposure
* auto white balance
* ISP
* qualquer processamento automático

Usar exposição fixa e ganho fixo.

========================
REQUISITOS DE PERFORMANCE
=========================

MUITO IMPORTANTE:

Evitar:

* malloc contínuo
* new/delete contínuos
* cópias desnecessárias
* std::vector crescendo
* conversões RGB
* OpenCV
* processamento pesado
* locks excessivos

Objetivo:

* pipeline extremamente leve
* previsibilidade temporal
* mínimo jitter

========================
RING BUFFER
===========

Criar um ring buffer circular de 150 frames.

Cada frame deve conter:

* frame_id uint64
* timestamp monotonic nanoseconds
* tamanho do payload
* ponteiro para bytes RAW10 packed
* width
* height

O buffer deve:

* ser totalmente pré-alocado
* nunca crescer dinamicamente
* sobrescrever os frames mais antigos continuamente

A captura nunca deve parar.

========================
ARQUITETURA
===========

Thread 1:

* captura contínua libcamera
* escreve no ring buffer

Thread 2:

* servidor TCP

Threads cliente:

* apenas leitura do ring buffer
* nunca acessam diretamente a câmera

========================
PROTOCOLO TCP
=============

Implementar protocolo simples texto/binário.

Comandos:

GET_LATEST

* retorna frame mais recente

GET_LAST N

* retorna últimos N frames

GET_FRAME ID

* retorna frame específico

========================
FORMATO DE TRANSMISSÃO
======================

Antes de cada frame enviar header binário contendo:

magic uint32
frame_id uint64
timestamp_ns uint64
width uint32
height uint32
payload_size uint32

Depois enviar:

* bytes RAW10 packed sem modificação

========================
REQUISITOS DE MEMÓRIA
=====================

Usar:

* buffers contíguos
* alinhamento eficiente
* preferência por memcpy mínimo
* evitar heap churn

Se possível:

* usar mmap/libcamera buffers diretamente

========================
REQUISITOS DE THREADING
=======================

* thread-safe
* baixa contenção
* mutex mínimo possível
* preferência para lock-free onde fizer sentido

========================
REQUISITOS DE DEBUG
===================

Exibir periodicamente:

* FPS real de captura
* tempo médio entre frames
* uso do ring buffer
* overruns
* dropped frames
* latência média

========================
REQUISITOS DE BUILD
===================

Gerar:

* CMakeLists.txt
* projeto organizado
* múltiplos arquivos .hpp/.cpp

Estrutura desejada:

/src
/include
/main.cpp
/CMakeLists.txt

========================
REQUISITOS DE QUALIDADE
=======================

Código:

* moderno C++17 ou C++20
* limpo
* altamente comentado
* robusto
* estilo industrial

========================
IMPORTANTE
==========

O sistema NÃO é uma câmera IP comum.

Ele é um:

* acquisition engine
* high-speed RAW capture server
* machine vision backend

Prioridade máxima:

* estabilidade temporal
* baixa latência
* 60 FPS sustentados
* eficiência de memória
* mínima cópia possível
* funcionamento contínuo 24/7
