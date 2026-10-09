# Ramona Granular - Sintetizador para PSP

**Ramona Granular** é um sintetizador granular polifônico com sequenciador integrado, desenvolvido em C nativo para o console PlayStation Portable (PSP). O projeto utiliza a biblioteca **SDL2** em conjunto com o **PSP SDK** para fornecer processamento de áudio em tempo real e renderização gráfica.

## 🛠 Funcionalidades Principais

*   **Motor de Síntese Granular:** Manipulação em tempo real de amostras de áudio, com controle de Tamanho do grão (Width), Posição inicial (Center), Pitch/Velocidade de reprodução e BPM. Inclui modos de varredura automática do áudio (Scan modes) para criar texturas evolutivas.
*   **Polifonia e Envelope:** O motor de áudio suporta 4 vozes de polifonia independentes. Possui controle total de envelope de volume no padrão ADSR (Attack, Decay, Sustain, Release).
*   **Sequenciador de 16 Passos:** Permite criar padrões rítmicos e melódicos ativando ou desativando 16 passos (steps). Cada passo possui ajuste individual de afinação (pitch) que varia de -24 a +24 semitons. A duração das notas pode ser subdividida (1, 1/2, 1/4, 1/8, 1/16).
*   **Rack de Efeitos (DSP):** A aba de Efeitos inclui 4 módulos processados simultaneamente:
    *   **Filtros:** High-Pass Filter (HPF) e Low-Pass Filter (LPF) com controles de Frequência de Corte, Volume e Panning espacial.
    *   **Delay:** Efeito de atraso com emulação de saturação de fita (Tape Saturation e Wobble), controlando Tempo (ms), Feedback (Eco) e Mix.
    *   **Reverb:** Reverberação customizável com parâmetros de Tamanho da sala, Amortecimento (Damp) e Mix.
*   **Navegador de Arquivos Integrado:** Lê arquivos `.wav` diretamente do Memory Stick do PSP, com capacidade de listar e ordenar até 48 amostras de áudio da pasta raiz. O sistema converte automaticamente taxas de amostragem variadas para o padrão do sintetizador (44100 Hz, Estéreo).
*   **Interface Estilizada:** O layout gráfico possui uma paleta de cores no estilo terminal/Ubuntu e é dividido em quatro abas intuitivas: Textura, Efeitos, Samples e Sequenciador. A interface principal conta também com um monitor de uso de CPU do console e uma animação interativa de fita cassete.

## 🎮 Controles do PSP

A interface foi projetada para ser controlada totalmente pelos botões físicos do console:

*   **Gatilhos L / R:** Alternam entre as 4 abas principais da interface.
*   **D-Pad (Setas Direcionais):** Navegam o cursor de seleção entre os diversos parâmetros, knobs e botões na tela.
*   **Analógico (Cima / Baixo):** Aumenta ou diminui o valor numérico do parâmetro que está atualmente focado.
*   **Botão X (Cruz):** Executa ações de clique/confirmação, como ligar/desligar módulos de efeito, ativar notas no sequenciador, abrir o navegador de arquivos (File Browser) ou carregar um sample selecionado.
*   **Botão Triângulo:** Ativa ou desativa a Modulação Automática (Auto-motion/Scan) do playhead na textura de áudio.
*   **Botão Select:** Alterna as subdivisões de tempo do sequenciador.
*   **Botão Start:** Dá Play ou Pausa no motor de áudio geral.
*   **Botão Círculo (Bolinha):** Fecha janelas modais, como o navegador de arquivos de áudio.

## ⚙️ Dependências e Compilação

Para compilar o código fonte e gerar o arquivo `EBOOT.PBP` para o seu PSP, você precisará ter o ambiente de desenvolvimento configurado:

1.  **PSPSDK (PSP Software Development Kit)**.
2.  **SDL2 compilada para o PSP**.
3.  Bibliotecas padrão de kernel, energia e controle do PSP (`pspkernel.h`, `pspctrl.h`, `psppower.h`, etc.).

Certifique-se de configurar o seu `Makefile` adequadamente vinculando a biblioteca SDL2 e o módulo `pspiofilemgr`. O arquivo principal de código-fonte referenciado no projeto é o `main.c`.

## 📂 Formato e Uso de Samples

*   Coloque seus arquivos `.wav` na mesma pasta onde o `EBOOT.PBP` for executado.
*   Para facilitar o teste inicial, o código pré-configura 8 slots (AMOSTRA 1 a AMOSTRA 8) buscando nomes de arquivo que vão de `sample.wav`, `sample2.wav` até `sample8.wav`. 
*   Você pode carregar arquivos com outros nomes acessando a aba "SAMPLES", selecionando "CARREGAR" e escolhendo o arquivo no navegador da interface.