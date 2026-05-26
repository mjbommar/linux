.PHONY: all report slides review clean

all: report slides

report:
	mkdir -p build/report
	cd report && latexmk -xelatex -interaction=nonstopmode -halt-on-error -outdir=../build/report uml-redesign-report.tex

slides:
	mkdir -p build/slides
	cd slides && latexmk -xelatex -interaction=nonstopmode -halt-on-error -outdir=../build/slides uml-redesign-slides.tex

review: all
	mkdir -p build/review/report build/review/slides
	pdftoppm -png -r 144 build/report/uml-redesign-report.pdf build/review/report/page
	pdftoppm -png -r 144 build/slides/uml-redesign-slides.pdf build/review/slides/slide
	magick montage build/review/report/page-*.png -thumbnail 260x360 -tile 4x -geometry +12+12 -background white build/review/report-contact.png
	magick montage build/review/slides/slide-*.png -thumbnail 280x160 -tile 5x -geometry +10+10 -background white build/review/slides-contact.png

clean:
	rm -rf build
