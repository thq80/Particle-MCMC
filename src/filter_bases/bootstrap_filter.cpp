#include "bootstrap_filter.h"

BSFilter::BSFilter(int numParts, BSResampStyle resampTechnique, unsigned int pathLength, double essPerc)
                : m_now(0), m_logLastCondLike(0.0), m_numParts(numParts), m_resampTechnique(resampTechnique), 
                  m_pathLength(pathLength), m_ESS(m_numParts), m_percentOfNumPartsThresh(essPerc)
{
    
    // make sure essPerc is a percent
    assert ((0.0 < essPerc) && (essPerc <= 1.0));
    
    // resize sample containers
    if (m_pathLength == 0)  // not storing whole paths, just filtering
    {
        m_particles.resize(1); // m_pathLength=0
        m_particles[0].resize(m_numParts);
    }
    else
    {
        m_particles.resize(m_pathLength);
        for(unsigned int jj = 0; jj < m_pathLength; ++jj)
            m_particles[jj].resize(m_numParts);
    }

    // set up log weights
    m_logUnNormWeights.resize(m_numParts); 

    //set all the weights to uniform
    std::vector<double>::iterator it;
    for(it = m_logUnNormWeights.begin(); it != m_logUnNormWeights.end(); ++it)
    {
        *it = 0.0;
    }
    
}

BSFilter::~BSFilter() {}


void BSFilter::filterOrSmooth(const Vec &dat, const std::vector<std::function<const Mat(const Vec&)> >& fs )
{
    if ( m_pathLength == 0 ){
        filter(dat, fs);   
    }else{
        smooth(dat, fs);
    }
}


void BSFilter::filter(const Vec &dat, const std::vector<std::function<const Mat(const Vec&)> >& fs) //TODO: no support for ESS stuff
{

    if (m_now == 0) //time 1
    {
    int timeSelector = 0;
       
        // initialize m_filtMean and m_dimState
        m_dimState = q1Samp(dat).rows();
       
        // only need to iterate over particles once
        //std::vector<double> currentLogWtAdjs(m_numParts);
        double sumWts(0.0);
        for(unsigned int ii = 0; ii < m_numParts; ++ii)
        {
            // sample particles
            m_particles[timeSelector][ii] = q1Samp(dat);
            m_logUnNormWeights[ii] = logMuEv(m_particles[timeSelector][ii]);
            m_logUnNormWeights[ii] += logGEv(dat, m_particles[timeSelector][ii]);
            m_logUnNormWeights[ii] -= logQ1Ev(m_particles[timeSelector][ii], dat);
                       
        }
       
        // calculate log cond likelihood with log-exp-sum trick
        std::vector<double>::iterator idxOfMax = std::max_element(m_logUnNormWeights.begin(), m_logUnNormWeights.end());
        double sumExp(0.0);
        for(unsigned int i = 0; i < m_numParts; ++i){
            sumExp += std::exp(m_logUnNormWeights[i] - (*idxOfMax));
        }
        m_logLastCondLike = -std::log(m_numParts) + (*idxOfMax) + std::log(sumExp);
   
        // calculate expectations before you resample
        m_expectations.resize(fs.size());
        std::fill(m_expectations.begin(), m_expectations.end(), Vec::Zero(m_dimState)); // TODO: should this be Mat::Zero(m_dimState, m_dimState)?
        int fId(0);
        for(auto & h : fs){
            double weightNormConst (0.0);
            for(size_t prtcl = 0; prtcl < m_numParts; ++prtcl){ // iterate over all particles
                m_expectations[fId] += h(m_particles[timeSelector][prtcl]) * std::exp(m_logUnNormWeights[prtcl]);
                weightNormConst += std::exp(m_logUnNormWeights[prtcl]);
            }
            m_expectations[fId] /= weightNormConst;
            fId++;
        }
   
   
        // resample if you should
        if (m_resampTechnique == BSResampStyle::everytime_multinomial)
            multinomRsmp(m_particles, m_logUnNormWeights);
   
        // advance time step
        m_now += 1;   
    }
    else // m_now > 0
    {

    int timeSelector = 0;
        int prevTime     = 0;
       
        // try to iterate over particles all at once
        std::vector<Vec> newSamps(m_numParts);
        std::vector<double> oldLogUnNormWts(m_logUnNormWeights);
        double currentLogWtAdjIndiv;       
        double maxOldLogUnNormWts(m_logUnNormWeights[0]);
        double sumWts(0.0);
        for(unsigned int ii = 0; ii < m_numParts; ++ii)
        {
            // sample and get weight adjustments
            newSamps[ii] = qSamp(m_particles[prevTime][ii], dat);
            currentLogWtAdjIndiv = logGEv(dat, newSamps[ii]);
 
            // update max of old logUnNormWts
            if (m_logUnNormWeights[ii] > maxOldLogUnNormWts)
                maxOldLogUnNormWts = m_logUnNormWeights[ii];
 
            // overwrite stuff
            m_logUnNormWeights[ii] += currentLogWtAdjIndiv;
            m_particles[timeSelector][ii] = newSamps[ii];

        }
       
        // compute estimate of log p(y_t|y_{1:t-1}) with log-exp-sum trick
        double maxNumer = *std::max_element(m_logUnNormWeights.begin(), m_logUnNormWeights.end()); //because you added log adjustments
        double sumExp1(0.0);
        double sumExp2(0.0);
        for(unsigned int i = 0; i < m_numParts; ++i){
            sumExp1 += std::exp(m_logUnNormWeights[i] - maxNumer);
            sumExp2 += std::exp(oldLogUnNormWts[i] - maxOldLogUnNormWts);
        }
        m_logLastCondLike = maxNumer + std::log(sumExp1) - maxOldLogUnNormWts - std::log(sumExp2);

        // calculate expectations before you resample
        m_expectations.resize(fs.size());
        std::fill(m_expectations.begin(), m_expectations.end(), Vec::Zero(m_dimState)); // TODO: should this be Mat::Zero(m_dimState, m_dimState)?
        int fId(0);
        for(auto & h : fs){ // iterate over all functions
            double weightNormConst (0.0);
            for(size_t prtcl = 0; prtcl < m_numParts; ++prtcl){ // iterate over all particles
                m_expectations[fId] += h(m_particles[timeSelector][prtcl]) * std::exp(m_logUnNormWeights[prtcl]);
                weightNormConst += std::exp(m_logUnNormWeights[prtcl]);
            }
            m_expectations[fId] /= weightNormConst;
            fId++;
        }

 
        // resample
        if (m_resampTechnique == BSResampStyle::everytime_multinomial)
            multinomRsmp(m_particles, m_logUnNormWeights);

        // advance time
        m_now += 1;       
    }
}

void BSFilter::smooth(const Vec &dat, const std::vector<std::function<const Mat(const Vec&)> >& fs) //TODO: no support for ESS stuff
{

    if (m_now == 0) //time 1
    {
    int timeSelector = m_now;
       
        // initialize m_filtMean and m_dimState
        m_dimState = q1Samp(dat).rows();
       
        // only need to iterate over particles once
        //std::vector<double> currentLogWtAdjs(m_numParts);
        double sumWts(0.0);
        for(unsigned int ii = 0; ii < m_numParts; ++ii)
        {
            // sample particles
            m_particles[timeSelector][ii] = q1Samp(dat);
            m_logUnNormWeights[ii] = logMuEv(m_particles[timeSelector][ii]);
            m_logUnNormWeights[ii] += logGEv(dat, m_particles[timeSelector][ii]);
            m_logUnNormWeights[ii] -= logQ1Ev(m_particles[timeSelector][ii], dat);
        }
       
        // calculate log cond likelihood with log-exp-sum trick
        std::vector<double>::iterator idxOfMax = std::max_element(m_logUnNormWeights.begin(), m_logUnNormWeights.end());
        double sumExp(0.0);
        for(unsigned int i = 0; i < m_numParts; ++i){
            sumExp += std::exp(m_logUnNormWeights[i] - (*idxOfMax));
        }
        m_logLastCondLike = -std::log(m_numParts) + (*idxOfMax) + std::log(sumExp);
   
        // calculate expectations before you resample
        m_expectations.resize(fs.size());
        std::fill(m_expectations.begin(), m_expectations.end(), Vec::Zero(m_dimState)); // fill everything with zero vctors
        int fId(0);
        for(auto & h : fs){
            double weightNormConst (0.0);
            for(size_t prtcl = 0; prtcl < m_numParts; ++prtcl){ // iterate over all particles
                m_expectations[fId] += h(m_particles[timeSelector][prtcl]) * std::exp(m_logUnNormWeights[prtcl]);
                weightNormConst += std::exp(m_logUnNormWeights[prtcl]);
            }
            m_expectations[fId] /= weightNormConst;
            fId++;
        }
   
   
        // resample if you should
        if (m_resampTechnique == BSResampStyle::everytime_multinomial)
            multinomRsmp(m_particles, m_logUnNormWeights);
   
        // advance time step
        m_now += 1;   
    }
    else // m_now > 0
    {
        int timeSelector = m_now;
        int prevTime     = m_now - 1;
       
    // try to iterate over particles all at once
        //std::vector<Vec> newSamps(m_numParts);
        //std::vector<double> currentLogWtAdjs(m_numParts);
        double currentLogWtAdjIndiv;
        std::vector<double> oldLogUnNormWts(m_logUnNormWeights);
        double maxOldLogUnNormWts(m_logUnNormWeights[0]);
        double sumWts(0.0);
        for(unsigned int ii = 0; ii < m_numParts; ++ii)
        {
            // sample and get weight adjustments
            m_particles[timeSelector][ii] = qSamp(m_particles[prevTime][ii], dat);
            currentLogWtAdjIndiv = logGEv(dat, m_particles[timeSelector][ii]);
 
            // update max of old logUnNormWts
            if (m_logUnNormWeights[ii] > maxOldLogUnNormWts)
                maxOldLogUnNormWts = m_logUnNormWeights[ii];
 
            // overwrite stuff
            m_logUnNormWeights[ii] += currentLogWtAdjIndiv;   

        }
       
        // compute estimate of log p(y_t|y_{1:t-1}) with log-exp-sum trick
        double maxNumer = *std::max_element(m_logUnNormWeights.begin(), m_logUnNormWeights.end()); //because you added log adjustments
        double sumExp1(0.0);
        double sumExp2(0.0);
        for(unsigned int i = 0; i < m_numParts; ++i){
            sumExp1 += std::exp(m_logUnNormWeights[i] - maxNumer);
            sumExp2 += std::exp(oldLogUnNormWts[i] - maxOldLogUnNormWts);
        }
        m_logLastCondLike = maxNumer + std::log(sumExp1) - maxOldLogUnNormWts - std::log(sumExp2);

        // calculate expectations before you resample
        m_expectations.resize(fs.size());
        std::fill(m_expectations.begin(), m_expectations.end(), Vec::Zero(m_dimState)); // fill everything with zero vctors
        int fId(0);
        for(auto & h : fs){ // iterate over all functions
            double weightNormConst (0.0);
            for(size_t prtcl = 0; prtcl < m_numParts; ++prtcl){ // iterate over all particles
                m_expectations[fId] += h(m_particles[timeSelector][prtcl]) * std::exp(m_logUnNormWeights[prtcl]);
                weightNormConst += std::exp(m_logUnNormWeights[prtcl]);
            }
            m_expectations[fId] /= weightNormConst;
            fId++;
        }

 
        // resample
        if (m_resampTechnique == BSResampStyle::everytime_multinomial)
            multinomRsmp(m_particles, m_logUnNormWeights);

        // advance time
        m_now += 1;       
    }
}


double BSFilter::getLogCondLike() const
{
    return m_logLastCondLike;
}


double BSFilter::getESS() const
{
    return m_ESS;
}


std::vector<std::vector<Vec> > BSFilter::getFullParts() const
{
    return m_particles;
}


std::vector<Mat> BSFilter::getExpectations() const
{
    return m_expectations;
}


std::vector<double> BSFilter::getLogUWeights() const
{
    return m_logUnNormWeights;
}


void BSFilter::multinomRsmp(std::vector<std::vector<Vec> > &oldParts, std::vector<double> &oldLogWeights)
{
    m_resampler.resampLogWts(oldParts, oldLogWeights);
}