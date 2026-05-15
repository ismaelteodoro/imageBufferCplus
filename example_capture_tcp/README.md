# example_capture_tcp

Cliente Python simples para consumir frames RAW10 do servidor `image_buffer_capture` pela rede.

O exemplo:

* conecta no Raspberry via TCP;
* envia `GET_LATEST`, `GET_LAST N` ou `GET_FRAME ID`;
* lê o header binário e o payload RAW10 packed;
* desempacota RAW10 para matriz 16-bit;
* gera uma imagem 8-bit para visualização;
* opcionalmente aplica debayer simples com OpenCV;
* exibe os frames na tela.

Instalação das dependências no cliente:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

Exemplo de uso:
```bash
python capture_client.py --host 203.0.113.10
```

```bash
python3 capture_client.py --host 192.168.1.50 --port 8000 --mode latest --count 20
```

Capturar os últimos 5 frames disponíveis no ring buffer:

```bash
python3 capture_client.py --host 192.168.1.50 --mode last --last 5
```

Buscar um frame específico:

```bash
python3 capture_client.py --host 192.168.1.50 --mode frame --frame-id 1234
```

Para deixar a imagem monocromática, sem debayer:

```bash
python3 capture_client.py --host 192.168.1.50 --no-debayer
```

Pressione `q` na janela para sair.
