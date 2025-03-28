# Jantar dos Filósofos

O Problema do Jantar dos Filósofos, formulado por Edsger Dijkstra em 1965, é um clássico da computação que ilustra desafios de concorrência e coordenação de recursos em sistemas distribuídos. Cinco filósofos estão sentados ao redor de uma mesa, cada um com um prato de comida e um garfo entre cada par adjacente. Para comer, um filósofo precisa usar dois garfos (o à sua esquerda e o à sua direita). O problema consiste em elaborar um protocolo que permita a todos comerem, evitando impasses (deadlocks) e garantindo justiça (nenhum filósofo passe fome indefinidamente).

Deadlock: Se todos os filósofos pegarem o garfo da esquerda simultaneamente, nenhum conseguirá o da direita, travando o sistema em um ciclo de espera infinita. Isso ocorre quando as quatro condições para deadlock (exclusão mútua, posse e espera, não preempção e espera circular) são atendidas.

Starvation (Inanição): Mesmo evitando deadlocks, um filósofo pode ser sistematicamente preterido, nunca conseguindo ambos os garfos. Isso exige mecanismos de justiça, como limites de tempo ou prioridades dinâmicas.

Concorrência Eficiente: Soluções ingênuas (como permitir que apenas um filósofo coma por vez) eliminam deadlocks, mas reduzem a eficiência. O ideal é maximizar a paralelização sem comprometer a segurança.

O garçom é responsável por autorizar ou negar o acesso aos garfos. Antes de pegar qualquer garfo, um filósofo deve pedir permissão ao garçom.

# Produtor Consumidor

O Problema do Produtor/Consumidor é um clássico da programação concorrente que ilustra os desafios de sincronização entre processos ou threads que compartilham um recurso comum. Nele, dois tipos de agentes atuam:

Produtores, que geram dados e os colocam em um buffer compartilhado (ex.: uma fila).

Consumidores, que retiram e processam esses dados do buffer.

Principais desafios:
Sincronização:

Exclusão mútua: Garantir que produtores e consumidores não acessem o buffer simultaneamente, evitando corrupção de dados.

Condições de contorno:

Se o buffer estiver cheio, os produtores devem esperar para adicionar novos dados.

Se o buffer estiver vazio, os consumidores devem esperar até que haja dados disponíveis.

Eficiência: Permitir que produtores e consumidores operem em paralelo sempre que possível, sem bloqueios desnecessários.

# Produtor Mutex

Resumo:
O código resolve o Problema do Produtor/Consumidor usando mutexes e condition variables para sincronizar o acesso a um buffer compartilhado.

Sincronização:
Mutex (buffer_mutex): Garante acesso exclusivo ao buffer.
Condition Variable (buffer_cond):
Produtores esperam se o buffer estiver cheio (buffer_size >= capacidade).
Consumidores esperam se o buffer estiver vazio (buffer_size == 0).
notify_all() acorda threads após inserção/remoção de itens.

Fluxo:

Produtores: Geram itens, checam buffer, e adicionam (com mutex travado).
Consumidores: Removem itens (com mutex travado) e processam.

Interface Gráfica:

Uma thread separada atualiza a tela com o estado do buffer, status das threads e logs.
Dados compartilhados são protegidos por um mutex global (g_state_mutex).

Prevenção de Erros:

Deadlock: Uso de while em wait() evita acordar prematuramente.
Race Conditions: Operações no buffer são atômicas.
Objetivo: Garantir acesso seguro ao buffer, evitar sobrecarga ou subutilização, e permitir visualização em tempo real do sistema.

# Filosofos Mutex

Explicação do Código:
Este código resolve o Problema do Jantar dos Filósofos usando mutexes e uma estratégia para evitar deadlocks. Funciona assim:

Sincronização dos Garfos:
Cada garfo é um mutex. Filósofos tentam "pegar" dois garfos (esquerdo e direito) usando lock().
⚠️ Prevenção de Deadlock: O último filósofo pega os garfos em ordem inversa (direita primeiro), quebrando a simetria circular.

Atualização do Estado:

Um mutex global (g_state_mutex) protege o estado compartilhado (g_state), que inclui status dos filósofos e garfos.

Uma thread de UI separada exibe o estado em tempo real usando ANSI escape codes.

Fluxo dos Filósofos:

Pensar → Tentar pegar garfos (com espera via lock()) → Comer → Liberar garfos (unlock()).

Logs e atualizações de status são registrados de forma thread-safe.

Interface Visual:

Exibe cores para estados (vermelho = esperando, verde = comendo).

Mostra garfos ocupados (▓) ou livres (░) e logs de atividades.

Objetivo: Permitir que filósofos comam sem deadlocks, com visualização clara do estado do sistema em tempo real.

# Corotinas

Explicação do Código de Corotinas com Scheduler:

Este código implementa um sistema de corotinas (funções que podem suspender e retomar execução) com um scheduler (agendador) em C++, utilizando primitivas de baixo nível como setjmp/longjmp e manipulação direta da pilha (stack). Veja como funciona:

Classe Coroutine:
Estado: Cada corotina tem um estado (READY, RUNNING, AWAITING, DONE).
Stack: Aloca uma região de memória própria para a pilha da corotina.
Função de entrada: Executa a função passada pelo usuário (CoroutineFunc).
Controle de contexto: Usa setjmp/longjmp para salvar/restaurar o contexto da CPU (registradores).
Suspensão (yield): Transfere controle de volta ao scheduler.
Sleep: Permite que a corotina durma por um tempo (sleep_for).

Classe Scheduler:
Gerencia corotinas: Mantém uma lista de corotinas e decide qual executar.
Inicialização: Prepara o stack de cada corotina e salva o contexto inicial.
Loop principal: Verifica corotinas prontas, acorda as que estão dormindo e alterna entre elas.
switch_stack:
Troca de pilha: Usa inline assembly para alterar o registrador rsp (stack pointer), permitindo que cada corotina tenha sua própria pilha.
Contexto seguro: Garante que a execução da corotina ocorra em seu stack isolado.

Fluxo de Execução:
Inicialização:
O scheduler cria as corotinas e aloca seus stacks.
Usa setjmp para salvar o contexto do scheduler e switch_stack para configurar o stack de cada corotina.
Execução de uma Corotina:
O scheduler seleciona uma corotina READY e usa longjmp para entrar em seu contexto.
A corotina roda até chamar yield() ou sleep_for(), que devolvem o controle ao scheduler via longjmp.

Suspensão e Retorno:
Quando uma corotina chama yield(), setjmp salva seu contexto e retorna ao scheduler.
Corotinas em AWAITING são reativadas quando seu tempo de espera expira.

# Jantar Coro

Este código resolve o Problema do Jantar dos Filósofos utilizando corotinas e controle cooperativo, evitando deadlocks e garantindo justiça. Funciona assim:

Gestão de Garfos Atômica:
A classe Table verifica e atribui ambos os garfos simultaneamente (request_forks). Se um garfo já estiver ocupado, o filósofo libera o que pegou e tenta novamente depois (back-off), evitando deadlock por espera circular.

Sincronização via Corotinas:
Cada filósofo é uma corotina que alterna entre estados (Pensando, Aguardando, Comendo). Quando um filósofo não consegue os garfos, ele cede controle (yield()) ao scheduler, permitindo que outros executem.

Prevenção de Starvation:
O scheduler alterna entre corotinas de forma não determinística, e os tempos de pensamento/comida são aleatórios, garantindo que nenhum filósofo seja permanentemente preterido.

Visualização em Tempo Real:
Atualizações do terminal mostram o estado dos filósofos e garfos, facilitando a depuração visual.

# Produtor Coro

Este código resolve o Problema do Produtor/Consumidor usando corotinas e controle cooperativo, garantindo sincronização sem race conditions. Funciona assim:

Gestão Segura do Buffer:

Produtores verificam se o buffer está cheio (is_full()) antes de inserir. Se cheio, entram em espera (yield()), cedendo controle ao scheduler.

Consumidores verificam se o buffer está vazio (is_empty()) antes de remover. Se vazio, também cedem controle.

Sincronização via Corotinas:

O scheduler alterna entre produtores/consumidores de forma não bloqueante.

Operações no buffer são atômicas (só uma corotina acessa o buffer por vez), evitando condições de corrida.

Prevenção de Starvation:

Tempos aleatórios de produção/consumo e alternância justa entre corotinas garantem que todos tenham acesso ao buffer.

Visualização em Tempo Real:
Atualizações no terminal mostram o estado do buffer e logs de atividades, facilitando a depuração.

Resumo: Usa corotinas para simular paralelismo seguro, com checks explícitos (cheio/vazio) e transferência cooperativa de controle (yield), resolvendo exclusão mútua e sincronização sem locks tradicionais.
