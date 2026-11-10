# GHL-PS4onPS3

Plugin para PS3 com CFW que permite jogar o **Guitar Hero Live de PS3** com a
**guitarra e o dongle de PS4** (USB `1430:07bb`).

Sem o plugin, o jogo para na tela *"Press any button on guitar controller"*:
ele só reconhece o dongle de PS3 (`12ba:074b`).

## Como funciona

O jogo fala com o dongle direto pela `libusbd`, registrando um driver próprio
(`GH_Guitar`) para o fabricante `0x12BA`. As funções de probe e attach desse
driver não conferem o VID/PID, só se o aparelho é HID com um endpoint de entrada
e um de saída, e o dongle de PS4 também é assim. Carregado dentro do jogo, o
plugin:

1. registra os **próprios callbacks do jogo** também para `1430:07bb`;
2. troca o comando periódico de ativação ("keep-alive") de PS3 pelo de PS4
   (`SET_REPORT 0x30`) e o repete a cada 8 s;
3. converte cada relatório de 64 bytes do PS4 para o layout de 27 bytes do PS3.

As chamadas `cellUsbdOpenPipe`, `cellUsbdControlTransfer` e
`cellUsbdInterruptTransfer` são interceptadas trocando o descritor de função
(OPD), a mesma técnica do ds45pad.

## Compatibilidade

- **Jogo:** Guitar Hero Live, `BLUS31556`, com a atualização 1.07. O plugin usa
  um endereço fixo desse executável e confere a assinatura antes de agir; em
  outra versão ele só registra `game_ops_not_found_wrong_eboot` no log e não
  faz nada.
- **Console:** CFW com Cobra e webMAN MOD (PS3MAPI). Testado no Evilnat 4.91.2.

## Mapeamento

| Guitarra | Dongle PS4 | Dongle PS3 |
|---|---|---|
| Brancos 1, 2, 3 | □, L1, R1 | □, L1, R1 |
| Pretos 1, 2, 3 | ×, ○, △ | ×, ○, △ |
| Palheta | analógico esq. Y | analógico esq. Y |
| Whammy | analógico dir. Y | analógico dir. Y |
| Tilt | analógico dir. X | acelerômetro X |
| Hero Power | R3 | Select |
| Pause | Options | Start |
| GHTV | L3 | L3 |
| Botão central | PS | PS |

Testado no console: trastes, palheta, tilt, Hero Power, Pause, GHTV e o botão PS
(que abre a XMB). O whammy segue o layout documentado, mas não pôde ser testado:
na guitarra usada nos testes, a alavanca não envia sinal nem no PC.

## Instalação

1. Copie `ghl_ps4onps3.sprx` para `/dev_hdd0/plugins/ghl/`.
2. Carregue o plugin **com o jogo aberto**, de um destes jeitos:
   - **Manual:** no navegador, `http://IP-DO-PS3/gameplugin.ps3mapi` → processo
     do jogo → caminho `/dev_hdd0/plugins/ghl/ghl_ps4onps3.sprx` → **Load**.
   - **Automático:** com o [ds45pad](https://github.com/domyno6773/DualSense-PS3)
     2.0.7 ou mais novo, crie `/dev_hdd0/plugins/ds45pad/game_plugins.txt`:
     ```
     BLUS31556 /dev_hdd0/plugins/ghl/ghl_ps4onps3.sprx
     ```
     O plugin é carregado sozinho cerca de 15 s depois que o jogo abre.
3. O dongle já conectado é reconhecido assim que o plugin carrega.

O log fica em `/dev_hdd0/tmp/ghl_ps4onps3.txt`.

## Compilação

Usa o Cell SDK (4.75) sob Wine e o `scetool`, como o ds45pad:

```
src/build_wine.sh          # versão normal
src/build_wine.sh debug    # registra no log os bytes desconhecidos do relatório
```

O build normal não escreve nada no HDD durante o jogo, só ao carregar e ao
conectar o dongle.

## Créditos e referências

- [PlasticBand](https://github.com/TheNathannator/PlasticBand): documentação
  dos relatórios dos dongles de PS3 e PS4.
- [RPCS3](https://github.com/RPCS3/rpcs3): emulação do dongle de PS3 (`GHLtar.cpp`).
- Driver `hid-sony` do Linux: comandos de ativação dos dongles.
- [ds45pad / DualSense-PS3](https://github.com/domyno6773/DualSense-PS3):
  `libc.c` mínima e a técnica de interceptação.

## Licença

GPL-2.0, a mesma do ds45pad, de onde vem a `libc.c`.
