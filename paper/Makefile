PDF=po3_pipeline.pdf
TEX=po3_pipeline.tex

all: $(PDF)

$(PDF): $(TEX)
	pdflatex -interaction=nonstopmode $(TEX)
	pdflatex -interaction=nonstopmode $(TEX)

clean:
	rm -f *.aux *.bbl *.blg *.log *.out *.pdf
