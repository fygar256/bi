bi: bi.c bi.py bi.1.gz
	chmod +x bi.py
	gcc bi.c -o bi -lm
	sudo cp bi /usr/local/bin/bi
	sudo cp bi.py /usr/local/bin/bip
	sudo cp bi.1.gz /usr/share/man/man1/
