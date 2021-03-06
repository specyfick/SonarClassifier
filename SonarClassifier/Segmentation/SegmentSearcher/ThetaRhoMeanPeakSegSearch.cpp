#include "ThetaRhoMeanPeakSegSearch.h"

#include "SegmentExtractor/SegmentExtractor.h"
#include "Segmentation.h"

#include "Drawing/Chart.h"

/**
 * @brief ThetaRhoMeanPeakSegSearch::ThetaRhoMeanPeakSegSearch
 * A ideia principal deste método é separar o fundo (background)
 * dos objetos de interesse.
 * Os objetos são mais eficientes do que o fundo para refletir as
 * ondas acústicas. Por este motivo eles são caracterizados por manchas
 * de alta intensidade nas imagens.
 * Para a sua detecção, nós utilizamos uma abordagem inspirada no processo
 * de formação das imagens acúticas para detectar
 * os picos de intensidade.
 * Cada beam acústico B é analisado individualmente, bin por bin.
 * A intensidade média I_mean(b,B) é calculada para cada bin b de um determinado
 * beam (B) através da equação X.
 *   I_mean(b,B) = 1/win_sz sum( I(i,B) , b-win_sz , b),
 * onde win_sz é o tamanho janela, em número de bins, utilizado no cálculo da média
 * ; b e i são identificadores de bin; B e o identificador do beam; I(i,B)
 *  é a intensidade do i^th bin do beam B^th beam.
 *  A intensidade I_peak(b,B) é um offset de I_mean(b,B) conforme mostra a equação
 * X.
 *   I_peak(b,B) = I_mean(b,B) + H_peak,
 *  onde H_peak é uma constante que determina a altura mínima de um pico de intensidade.
 * Os bins em sequência com intensidade I(b,B) superior a I_peak(b,B) são considerados
 * parte de um pico de intensidade. Nesta sequência, o pico é representado pelo bin b_peak
 * com maior intensidade I(b_peak,B).
 *  A Figura 4a mostra em vermelho os valores de I_mean(b,B), em azul os valores
 * I(b,B) e em verde os valores I_peak(b,B) de todos os bins de um único beam B.
 * Os picos detectados b_peak estão representados por circulos coloridos.
 *  Os picos b_peak detectados são definidos pela quadrupla (x,y,I(b_peak,B),I_mean(b_peak,B)),
 * onde x,y é a posição do bin b_peak na imagem.
 *  Após a detecção de todos os picos da imagem, uma busca por pixels conexos é
 * realizada para cada pico, iniciando no pico de menor intensidade I(b_peak,B) até o de maior intensidade.
 *  A conexão de 8 vias é utilizada como critério de vizinhança pelo algoritmo BFS. Nesta busca,
 * todos os pixels conexos são visitados obedecendo o seguinte critério: A sua intensidade I(b,B)
 *  precisa ser superior a intensidade do pico I_mean(b_peak,B), caso contrário
 *  a sua distância relativa a borda do segmento deve ser inferior ao parâmetro D_seg
 * em pixels.
 *
 *  O critério de distância foi adotado para reduzir o problema de multisegmentação
 *  de um único objeto, causado quando um grupo de pixels de alta intensidade
 * é dividido por pixels de baixa intensidade. Este efeito é causado pelo ruído
 * ou pelas sombras acústicas. A Figura 5 mostra o comportamento do algoritmo de segmentação
 * ao alterar o parâmetro D_seg.
 *
 */

ThetaRhoMeanPeakSegSearch::ThetaRhoMeanPeakSegSearch():
    nBeams(720),startBin(20),Hmin(110),bearing(130.f),
    sonVerticalPosition(1),
    minSampleSize(10), meanWindowSize(5)
{

}

void ThetaRhoMeanPeakSegSearch::segment(Mat &img16bits, vector<Segment *> *sg)
{
    unsigned nBins = img16bits.rows-startBin;

    float beamRadIncrement, radBearing = bearing*M_PI/180.f,
          currentRad = -radBearing/2.f;

    // Compute ang variation
    if(nBeams>1)
        beamRadIncrement = radBearing/(nBeams-1);
    else beamRadIncrement = 2*radBearing; // Infinity!

#ifdef SEGMENTATION_DRWING_DEBUG
//    Mat result(img16bits.rows, img16bits.cols, CV_8UC3, Scalar(0,0,0));

    Mat result;
    img16bits.convertTo(result,CV_8UC1);
    cvtColor(result,result,CV_GRAY2BGR);
#endif

    // Sonar position botton middle of the image
    Point2f sonarPos(img16bits.cols/2.f, img16bits.rows+sonVerticalPosition);

    vector<pair<int , unsigned> > peaks;
    vector<Point2f> peaksPostions;
    peaks.reserve(3000);
    peaksPostions.reserve(3000);

    CircularQueue lastBins(meanWindowSize);

    // Bin peak search
    // For each beam
    for(unsigned beam = 0; beam < nBeams; beam++, currentRad+=beamRadIncrement)
    {
        float sinRad = sin(currentRad),
              cosRad = cos(currentRad);

        Point2f binPos(sonarPos.x - startBin*sinRad,
                        sonarPos.y - startBin*cosRad),
                binStartPos(binPos);

        Point2f beamDir(-sinRad,-cosRad);

        int ACCIntensity = 0,
            maxHeight = 0, maxHBin = -1,
            minHeight = 99999, minHBin=-1;
        bool onPeak=false; // True if we are on a intensity peak

        lastBins.clear();

        // For each bin
        for(unsigned bin = 0; bin < nBins; bin++)
        // Unsigned overflow!
        {
            binPos+= beamDir;

            // Get bin intensity
            int binI = img16bits.at<ushort>(binPos.y,binPos.x),
                meanIntensity=0,
                peakHeight;

            // Compute mean intensity
            if(lastBins.size() > 0)
                meanIntensity = ACCIntensity/lastBins.size();

            // Compue peak height
            peakHeight = binI - meanIntensity;

            // Verity if this intensity is a peak
            if(peakHeight > Hmin)
            {
                if(!onPeak)
                {   // We are entering on a intensity peak
                    minHeight = maxHeight = peakHeight;
                    minHBin = maxHBin = bin;
                    onPeak = true;
                }else
                {
                    if(peakHeight > maxHeight)
                    {
                        maxHeight = peakHeight;
                        maxHBin = bin;
                    }else if(peakHeight < minHeight)
                    {
                        minHeight = peakHeight;
                        minHBin = bin;
                    }
                }
            }else if(onPeak)
            {// End of peak analisy

                // Compute threshold (take minHeight in acount)
                int threshold = meanIntensity+minHeight;
//cout << "Meam " << meanIntensity << " th " << threshold << endl;

                // Compute the peak position on sonar XY image (take maxHeight in acount)
                Point2f peakPosition(binStartPos + beamDir * maxHBin);
//                cout << "px " << peakPosition.x
//                     << " py " << peakPosition.y
//                     << " bin " << maxHBin
//                     << endl;

                peaks.push_back(pair<int,unsigned>(threshold,peaksPostions.size()));
                peaksPostions.push_back(peakPosition);

                onPeak = false;

                maxHeight = 0;
                maxHBin = -1;
            }

            if(!onPeak)
            {
                // Save current bin
                if(lastBins.size() >= meanWindowSize)
                { // Remove older bin
                    ACCIntensity -= lastBins.front();
                    lastBins.pop();
                }
                // Add newer bin
                ACCIntensity+= binI;
                lastBins.push(binI);
            }

            // Debug stuffs
            #ifdef SEGMENTATION_DRWING_DEBUG
                // Mark on resulted img the position of visited pixel
//                result.at<Vec3b>(binPos.y,binPos.x)[0] = Drawing::color[beam%Drawing::nColor].val[0];
//                result.at<Vec3b>(binPos.y,binPos.x)[1] = Drawing::color[beam%Drawing::nColor].val[1];
//                result.at<Vec3b>(binPos.y,binPos.x)[2] = Drawing::color[beam%Drawing::nColor].val[2];
            #endif
        }
    }

    // ====== Segmentation step =========

    /* Initialize Mask of Visit */
    m_seg->resetMask(img16bits.rows,img16bits.cols);
    sg->clear();

    /* Search for high intensity pixels */
    unsigned segCount=0;
    Segment *seg=0x0;

//    cout << "Size = " << peaks.size() << endl;

    // Sort in incrise order of thresholds
    sort(peaks.begin(),peaks.end());

    for(unsigned i = 0 ; i < peaks.size() ; i++)
    {
        // Search the segment on image
        seg = m_seg->segment(segCount);

        pair<int,unsigned> &peak = peaks[i];
        Point2f &peakPosition = peaksPostions[peak.second];

        m_extractor->setThreshold(peak.first);
        m_extractor->createSegment(seg,img16bits,
                                   peakPosition.y,peakPosition.x);

        // If segment is greater tham minimum acceptable segment size
        if(seg->N >= minSampleSize)
        {
            segCount++;

            #ifdef SEGMENTATION_DRWING_DEBUG
                seg->drawSegment(result,Drawing::color[segCount%Drawing::nColor]);
            #endif

            // Add seg to answer
            sg->push_back(seg);
        }
    }

    #ifdef SEGMENTATION_DRWING_DEBUG
    imshow("TR ThetaRhoMeanPeakSegSearch image result", result);
    #endif

}

void ThetaRhoMeanPeakSegSearch::load(ConfigLoader &config)
{
    int vi;
    float vf;

    if(config.getInt("General","MinSampleSize",&vi))
    {
        minSampleSize = vi;
    }

    if(config.getInt("ThetaRhoMeanPeakSegSearch","sonVerticalPosition",&vi))
    {
        sonVerticalPosition = vi;
    }

    if(config.getInt("ThetaRhoMeanPeakSegSearch","minSampleSize",&vi))
    {
        minSampleSize = vi;
    }

    if(config.getInt("ThetaRhoMeanPeakSegSearch","nBeams",&vi))
    {
        nBeams = vi;
    }

    if(config.getInt("ThetaRhoMeanPeakSegSearch","startBin",&vi))
    {
        startBin = vi;
    }

    if(config.getInt("ThetaRhoMeanPeakSegSearch","Hmin",&vi))
    {
        Hmin = vi;
    }

    if(config.getFloat("ThetaRhoMeanPeakSegSearch","bearing",&vf))
    {
        bearing = vf;
    }

    if(config.getInt("ThetaRhoMeanPeakSegSearch","meanWindowSize",&vi))
    {
        meanWindowSize = vi;
    }

}

void ThetaRhoMeanPeakSegSearch::calibUI(Mat &img16bits)
{
    unsigned nBins = img16bits.rows-startBin;

    float beamRadIncrement, radBearing = bearing*M_PI/180.f,
          currentRad = -radBearing/2.f;

    // Compute ang variation
    if(nBeams>1)
        beamRadIncrement = radBearing/(nBeams-1);
    else beamRadIncrement = 2*radBearing; // Infinity!

#ifdef CALIB_SEGMENTATION_DRWING_DEBUG
    Mat result(img16bits.rows, img16bits.cols, CV_8UC3, Scalar(0,0,0));

//    Mat result;
//    img16bits.convertTo(result,CV_8UC1);
//    cvtColor(result,result,CV_GRAY2BGR);
#endif

    // Images to show temporary results
    Mat mPlot,mask;
    Chart chart(img16bits.cols*0.7,img16bits.rows*0.7);
    char tmpStr[100];

    // Sonar position botton middle of the image
    Point2f sonarPos(img16bits.cols/2.f, img16bits.rows+sonVerticalPosition);

    vector<pair<int , unsigned> > peaks;
    vector<Point2f> peaksPostions;
    peaks.reserve(3000);
    peaksPostions.reserve(3000);

    CircularQueue lastBins(meanWindowSize);

    while (true)
    {
        // Create plots (It will be cleaned at end)
        unsigned meanPlot = chart.newLabel(Chart::PLOT_CONTINUOS_LINE,Scalar(0,0,255),1),
                 binsPlot = chart.newLabel(Chart::PLOT_CONTINUOS_LINE,Scalar(255,0,0),1),
                 acceptCounstraintLinePlot = chart.newLabel(Chart::PLOT_CONTINUOS_LINE,Scalar(0,255,0),1);

         m_seg->resetMask(img16bits.rows,img16bits.cols);

        float sinRad = sin(currentRad),
              cosRad = cos(currentRad);

        Point2f binPos(sonarPos.x - startBin*sinRad,
                        sonarPos.y - startBin*cosRad),
                binStartPos(binPos);

        Point2f beamDir(-sinRad,-cosRad);

        int ACCIntensity = 0,
            maxHeight = 0, maxHBin = -1,
            minHeight = 99999, minHBin=-1;

        bool onPeak=false;
        lastBins.clear();

        img16bits.convertTo(mask,CV_8UC1);
        cvtColor(mask,mask,CV_GRAY2BGR);

//        line(mask,
//             binPos,
//             binPos+((float)nBins*beamDir),Scalar(255,0,0),2);

        // For each bin
        for(unsigned bin = 0; bin < nBins; bin++)
        // Unsigned overflow!
        {
            binPos+= beamDir;

            // Draw anlised beam on image
            mask.at<Vec3b>(binPos.y,binPos.x)[0] = Drawing::color[0].val[0];
            mask.at<Vec3b>(binPos.y,binPos.x)[1] = Drawing::color[0].val[1];
            mask.at<Vec3b>(binPos.y,binPos.x)[2] = Drawing::color[0].val[2];

            // Get bin intensity
            int binI = img16bits.at<ushort>(binPos.y,binPos.x),
                meanIntensity=binI, // A initial guess
                peakHeight;

            // Plot bin intensity
            chart.addPoint(binsPlot,bin,binI);

            // Compute mean intensity
            if(lastBins.size() > 0)
                meanIntensity = ACCIntensity/lastBins.size();

            // Plot mean intensity
            chart.addPoint(meanPlot,bin,meanIntensity);

            // Plot accept peak line
            chart.addPoint(acceptCounstraintLinePlot,bin,meanIntensity+Hmin);

            // Compue peak height
            peakHeight = binI - meanIntensity;

            // Verity if it's intensity is a peak
            if(peakHeight > Hmin)
            {

                if(!onPeak)
                {
                    minHeight = maxHeight = peakHeight;
                    minHBin = maxHBin = bin;
                    onPeak = true;

                }else
                {
                    if(peakHeight > maxHeight)
                    {
                        maxHeight = peakHeight;
                        maxHBin = bin;
                    }else if(peakHeight < minHeight)
                    {
                        minHeight = peakHeight;
                        minHBin = bin;
                    }
                }
            }else if(onPeak)
            {// End of peak analisy

                // Compute threshold (take minHeight in acount)
                int threshold = meanIntensity+minHeight;

                // Compute the peak position on sonar XY image (take maxHeight in acount)
                Point2f peakPosition(binStartPos + beamDir * maxHBin);

                // Save detected peak
                peaks.push_back(pair<int,unsigned>(threshold,peaksPostions.size()));
                peaksPostions.push_back(peakPosition);

                // Ploat detected peak
                chart.newLabel(Chart::PLOT_CIRCLE,Drawing::color[(peaksPostions.size()-1)%Drawing::nColor],-1);
                chart.addPoint(maxHBin,meanIntensity + maxHeight); // PLot circle

                chart.newLabel(Chart::PLOT_CIRCLE,Scalar(0,0,0),1);
                chart.addPoint(maxHBin,meanIntensity + maxHeight); // PLot circle contour

                onPeak = false;
            }

            if(!onPeak)
            {
                // Save current bin
                if(lastBins.size() >= meanWindowSize)
                { // Remove older bin
                    ACCIntensity -= lastBins.front();
                    lastBins.pop();
                }
                // Add newer bin
                ACCIntensity+= binI;
                lastBins.push(binI);
            }

            // Debug stuffs
            #ifdef CALIB_SEGMENTATION_DRWING_DEBUG
                // Mark on resulted img the position of visited pixel
//                result.at<Vec3b>(binPos.y,binPos.x)[0] = Drawing::color[beam%Drawing::nColor].val[0];
//                result.at<Vec3b>(binPos.y,binPos.x)[1] = Drawing::color[beam%Drawing::nColor].val[1];
//                result.at<Vec3b>(binPos.y,binPos.x)[2] = Drawing::color[beam%Drawing::nColor].val[2];
            #endif
        }

        // ====== Segmentation step =========

        // Sort in incrise order of thresholds
        sort(peaks.begin(),peaks.end());

        for(unsigned i = 0 ; i < peaks.size() ; i++)
        {
            // Get a seg
            Segment *seg= m_seg->segment(0);

            pair<int,unsigned> &peak = peaks[i];
            Point2f &peakPosition = peaksPostions[peak.second];

            // Search for segment on image
            m_extractor->setThreshold(peak.first);
            m_extractor->createSegment(seg,img16bits,
                                       peakPosition.y,peakPosition.x);

            // Draw segment on image result
            seg->drawSegment(mask,Drawing::color[peak.second%Drawing::nColor]);

            circle(mask,peakPosition,7,Drawing::color[peak.second%Drawing::nColor],-1);
            circle(mask,peakPosition,7,Scalar(0,0,0),1);

            #ifdef CALIB_SEGMENTATION_DRWING_DEBUG
                seg->drawSegment(result,Drawing::color[peak.second%Drawing::nColor]);
            #endif
        }

        peaks.clear();
        peaksPostions.clear();

        #ifdef CALIB_SEGMENTATION_DRWING_DEBUG
        imshow("TR ThetaRhoMeanPeakSegSearch image result", result);
        #endif


        imshow("TR image mask", mask);
        Drawing::plot(chart,mPlot);
        imshow("TR plot", mPlot);
        chart.clear();

        // Keyboar input
        char c = waitKey();
        switch(c)
        {
        case 'a':
            if(currentRad<65.f*M_PI/180.f)
                currentRad+=1.f*M_PI/180.f;
        break;
        case 'd':
            if(currentRad>-65.f*M_PI/180.f)
                currentRad-=1.f*M_PI/180.f;
        break;
        case 'w':
            Hmin+=2;
        break;
        case 's':
            if(Hmin>=2)
                Hmin-=2;
        break;

        case 'r':
            meanWindowSize+=1;
            lastBins.ResizeAndClear(meanWindowSize);
            cout << "meanWindowSize = " << meanWindowSize << endl;
        break;
        case 'f':
            if(meanWindowSize > 0)
            {
                meanWindowSize-=1;
                lastBins.ResizeAndClear(meanWindowSize);
            }
            cout << "meanWindowSize = " << meanWindowSize << endl;
        break;

        case 'b':
//            setlocale(LC_ALL, "C"); // USE POINT AS DECIMAL SEPARATOR!
//            sprintf(str,"TRPeak_PiR%.2f_PiF%.2f_Hb%u.png",
//                    PiRecursive,PiEnd,Hmin);

//            cout << "Saving img " << str << endl;
//            imwrite(str,mPlot);

//            cout << "Saving img " << str << endl;
//            sprintf(str,"TRImg_PiR%.2f_PiF%.2f_Hb%u.png",
//                    PiRecursive,PiEnd,Hmin);
//            imwrite(str,mask);

        break;
        case 'p':
            sprintf(tmpStr,"CalibResult_B%.1f_Hp%d_Wsz%d.png",
                    currentRad*180.f/M_PI,Hmin,meanWindowSize);

            imwrite(tmpStr,mask);

            sprintf(tmpStr,"CalibPlot_B%.1f_Hp%d_Wsz%d.png",
                    currentRad*180.f/M_PI,Hmin,meanWindowSize);

            imwrite(tmpStr,mPlot);

        break;
        case 10: case 13: case 27: // keys Enter or ESC
            destroyWindow("TR image mask");
            destroyWindow("TR plot");
            return;
        break;
        }
        cout << "bearign " << (currentRad*180.f/M_PI) << " Hmin " << Hmin << endl;
    }
}
