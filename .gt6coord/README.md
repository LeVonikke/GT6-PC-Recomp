# Coordenação entre agentes

Este diretório é o protocolo compartilhado para agentes que trabalham no GT6.
Não é código do jogo.

## Regras

1. Antes de iniciar uma tarefa, registre-a como `claimed` em `tasks.json` e
   relacione os arquivos que serão modificados.
2. Um arquivo listado em uma tarefa `claimed` não pode ser alterado por outro
   agente. Use uma tarefa de análise sem `files` quando não houver edição.
3. Ao terminar, registre evidência reproduzível, arquivos alterados e o
   próximo bloqueador. Marque a tarefa como `done` ou `blocked`.
4. O agente que executou um build ou teste registra comando, diretório de
   saída e resultado antes de liberar a tarefa.
5. `historico_ia.txt` continua sendo o handoff humano durável. Atualize-o
   apenas depois de uma conclusão validada.

## Estados

`queued` -> `claimed` -> `done` | `blocked`.

Uma tarefa de análise pode ser feita em paralelo com uma tarefa de código,
desde que `files` permaneça vazio e ela não execute builds concorrentes.
