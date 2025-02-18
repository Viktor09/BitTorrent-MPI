Incepem cu structurile:

SwarmClient reprezinta un client in swarm cu urmatoarele fielduri:
rank-ul adica care client
type-ul seed/peer 0/1
filename-ul numele fisierului

Am si un comparator in structura pentru a tine instantele ordonate
Si e facuta in ordine, rank, tip, filename

Am si niste mapuri si set-uri pentru prelucrarea datelor, adica
next_owner_index este pentru eficienta am folosit round-robin,
files_available contine hash-urile fisierelor disponibile,
achived_files contine hash-urile fisierelor detinute de clienti.

swarm este un set de clienti in swarm,
wanted_files este un set de fisiere dorite de clienti.

Dupa care urmeaza citirea din fisier si bagarea datelor in cele doua structuri achived_files si wanted_files explicate mai sus.

Dupa care am scrierea in fisier a hash-urilor corespunzator enuntului.

Functia get_next_owner() determina urmatorul proprietar al unui fisier folosind un algoritm round-robin pentru eficienta. Verifica daca lista de proprietari este goala, initializeaza indexul daca este necesar, si gaseste urmatorul proprietar care nu este selfRank, actualizand indexul. Returneaza rank-ul proprietarului sau -1 daca nu exista unul valid.

send_swarm_update trimite() un update request catre tracker pentru a primi lista updata de clienti care detin un fisier.

request_file_info() cere informatii despre un fisier si primeste ownerii si hash-urile.

download_segment() incearca sa descarce un hash al unui fisier de la proprietari. Trimite cereri MPI pentru upload si hash, primeste statusul si hash-ul segmentului si actualizeaza lista de hash-uri descarcate. Returneaza true daca segmentul a fost descarcat cu succes, altfel false.

download_file() primeste hash-urile downloadate si le scrie in fisier-ul de output.

download_thread_func() apeleaza functiile request_file_info() si download_file() despre care am vb mai sus.

handle_upload_request() gestioneaza cererile de upload de la alti clienti. Primeste numele fisierului si hash-ul solicitat, verifica daca hash-ul este detinut de client, si trimite un cod de status inapoi. Daca hash-ul este detinut, trimite si hash-ul solicitat.

upload_thread_func() asculta requesturi de upload si le da handle.

handle_swarm_update() updateaza swarm-ul cu informatii noi de la 
clienti.

receive_file_info() primeste informatiile despre un fisier de la un
client si updateaza swarn-ul si adauga in file_available date despre
disponibilitatea lor.

send_ready_to_start() trimite mesaje clientilor ca tracker-ul e gata
sa inceapa procesul de partajare din tracker.

handle_file_request() gestioneaza cererile de fisiere de la clienti. Primeste numele fisierului solicitat, gaseste proprietarii fisierului in swarm, si trimite lista de proprietari si hash-urile fisierului inapoi catre clientul solicitant.

handle_finish() asigura mesajele de finalizare de la clienti. Primeste un mesaj de finalizare de la un client si incrementeaza contorul de clienti finalizati.

handle_download_complete() se ocupa cu mesajele de descarcare completa de la clienti. Primeste numele fisierului descarcat, actualizeaza statusul clientului din peer in seed in swarm si inlocuieste vechiul peer cu noul seed.

send_finish_message() trimite mesajele de finalizare catre toti clienti, ca trackerul a terminat de procesat.

tracker() coordoneaza tot ce am descris mai sus in functie de request.

send_file_info() trimite informatiile despre fisierul primit in peer la tracker.

peer() aici efectiv doar citesc datele si le trimit la tracker plus ce era in schelete.

Observatii: mi-am definit niste taguri unice, in functie de necesitatea de request pentru a trimite mesajele corect intre diferite componente.