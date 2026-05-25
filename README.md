# LoRa Mesh Chat

*🇧🇷 Português (BR) · [🇺🇸 English](README.en.md)*

Um chat de texto que funciona **sem internet, sem operadora e sem app**. Você liga a
placa, conecta o celular no Wi-Fi dela, abre o navegador e conversa. As mensagens
viajam por **rádio LoRa** de uma placa pra outra — o Wi-Fi serve só pra mostrar a
tela no seu telefone.

Comecei isso porque comprei um kit de duas placas LoRa e o firmware que veio era
todo amarrado a um aplicativo. Eu queria o contrário: liga e usa, de qualquer
celular, sem instalar nada. Então reaproveitei um chat por Wi-Fi que eu tinha
feito anos atrás pra um ESP8266 e reescrevi tudo pra essas placas, trocando o
transporte das mensagens de Wi-Fi pra LoRa.

> Funciona de verdade — os prints lá embaixo são de duas placas conversando.

## Como funciona, em uma frase

A placa cria a própria rede Wi-Fi e serve as páginas a partir da memória dela. O
que sai e entra de mensagem, porém, passa por LoRa entre as placas, formando uma
malha (mesh) onde todo mundo retransmite pra todo mundo.

## Telas

Tela inicial — escolhe entre entrar numa malha existente ou virar o administrador:

![Tela inicial](docs/img/01-home.webp)

Conversa do lado de quem conectou (chat geral e conversas privadas por placa):

![Chat do cliente](docs/img/02-chat-cliente.webp)

Painel do administrador — quem hospeda pode autorizar, expulsar e bloquear placas:

![Painel do administrador](docs/img/03-painel-admin.webp)

## O que dá pra fazer

- Chat geral, onde todo mundo da malha recebe.
- Conversa privada (DM) com uma placa específica.
- Vários celulares no mesmo Wi-Fi da placa, cada um com seu próprio nome.
- Quem escolhe "Hospedar" vira administrador e pode autorizar, expulsar e bloquear.
- Baixar o histórico da conversa em `.txt`.
- Tudo offline. Nenhum servidor, nenhuma nuvem, nenhum cadastro.

## Hardware

Feito e testado na **Heltec WiFi LoRa 32 V3** (ESP32-S3 + rádio LoRa SX1262).
Você precisa de **pelo menos duas** pra ter conversa. Lembre da antena de 868 MHz
(Europa) ou 915 MHz (EUA) — e ligue a antena **antes** de energizar a placa, senão
você corre o risco de queimar o rádio.

## Instalando (Arduino IDE)

1. **Suporte ao ESP32:** Ferramentas → Placa → Gestor de Placas → procure por
   `esp32` (Espressif) e instale.
2. **Selecione a placa:** Ferramentas → Placa → `Heltec WiFi LoRa 32(V3)`.
3. **Bibliotecas** (Ferramentas → Gerir Bibliotecas):
   - `RadioLib` — by jgromes
   - `ESPAsyncWebServer` — **by mathieucarbou**
   - `AsyncTCP` — **by mathieucarbou**
   - `ArduinoJson` — by bblanchon

   > Importante: a `ESPAsyncWebServer` e a `AsyncTCP` **têm que ser as duas do
   > mathieucarbou**. Misturar versões de autores diferentes dá um erro de
   > compilação chato (`discards qualifiers`). Se acontecer, apague as duas pastas
   > em `Arduino/libraries/` e reinstale as do mathieucarbou.

4. **Frequência:** o padrão é 868 MHz. Nos EUA, mude `LORA_FREQ` pra `915.0` lá no
   topo do `.ino`.
5. Abra `LoRaMeshChat.ino`, escolha a porta e clique em **Carregar**.
6. Repita pra segunda placa. **Não precisa mudar nada** — cada placa gera o próprio
   ID a partir do número de série dela.

## Usando

1. No celular, conecte no Wi-Fi `LoRaMesh-XXXX`. Senha: `Zeus6996`.
2. Abra `http://192.168.1.1`.
3. Numa placa escolha **Hospedar**, na outra escolha **Conexão**.
4. Em "Conexão", espere a outra placa aparecer na lista e toque em **Conectar**.
5. Coloque um nome e comece a conversar.

> A senha e o nome da rede ficam no topo do `.ino`, é só trocar se quiser.

## Por que LoRa não tem "lista de redes" como o Wi-Fi

No Wi-Fi você vê as redes porque elas ficam se anunciando. No LoRa não existe isso
de fábrica. Pra resolver, cada placa manda um pequeno **beacon** (sinal de "tô
aqui") de tempos em tempos. Quando você toca em "Conexão", a placa fica ouvindo
esses beacons por alguns segundos e mostra quem respondeu. É isso que faz a coisa
ser plug and play.

## Sobre o "administrador"

Numa malha LoRa não tem servidor central obrigatório — todas as placas falam com
todas. Quem "hospeda" é o **moderador**: autoriza, expulsa e bloqueia. As mensagens
continuam chegando em todo mundo, mesmo que o host esteja longe, porque as placas
retransmitem umas pelas outras (flood com limite de saltos). Se duas placas
tentarem hospedar ao mesmo tempo, **a que reivindicou primeiro vence** e a outra
cede sozinha.

## Coisas que eu não escondo de você

- LoRa é **lento e de pouca banda**. É ótimo pra texto curto. Não espere mandar
  foto nem mensagem gigante.
- O histórico fica na memória RAM (umas 120 mensagens). Reiniciou a placa, limpou
  o histórico — por isso existe o botão de baixar.
- A **bateria não dura dias** com isso, e o motivo é o Wi-Fi ligado o tempo todo
  (é ele que come energia, não o LoRa). Numa bateria de 1100 mAh, espere algo na
  faixa de poucas horas de uso ativo. Dá pra melhorar muito colocando a placa pra
  dormir e só acordar o Wi-Fi quando precisar, mas isso muda o "sempre acessível".
- O OLED vem **desligado de propósito** pra poupar energia. Pra reativar, mude
  `USE_OLED` pra `1` e instale a biblioteca `U8g2`.

## Ideias pra quem quiser mexer

- Wi-Fi sob demanda (placa dorme, botão acorda) pra esticar a bateria.
- Salvar histórico na flash pra sobreviver a reinício.
- Criptografia das mensagens no ar.
- Confirmação de entrega (ACK) e reenvio.

## Licença

MIT. Use, modifique, faça o que quiser. Se melhorar, manda um PR. :)
