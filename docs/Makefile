PUML_SRC  := $(wildcard *.puml)
SVG_OUT   := $(patsubst %.puml,docs_bin/%.svg,$(PUML_SRC))
PNG_OUT   := $(patsubst %.puml,docs_bin/%.png,$(PUML_SRC))

.PHONY: all docs pdf clean

all: pdf

docs: $(SVG_OUT)

docs_bin/%.svg: %.puml | docs_bin
	plantuml -tsvg -o docs_bin $<

docs_bin/%.png: docs_bin/%.svg
	rsvg-convert -f png -d 150 -p 150 $< -o $@

docs_bin:
	mkdir -p docs_bin

docs_bin/nobg-light-logo.png: ../static/nobg-light-logo.svg | docs_bin
	rsvg-convert -f png -d 300 -p 300 $< -o $@

pdf: docs $(PNG_OUT) docs_bin/nobg-light-logo.png
	pdflatex -output-directory=docs_bin greenbox-docs.tex
	pdflatex -output-directory=docs_bin greenbox-docs.tex

clean:
	rm -rf docs_bin/*
