#ifndef SVGPARSER_H
#define SVGPARSER_H

#include <QHash>
#include <QString>
#include <QDomNode>
#include <QDomElement>

#include "skin/skincontext.h"


// A class for managing the svg files
class SvgParser {
  public:
    SvgParser();
    SvgParser(const SkinContext& parent);
    virtual ~SvgParser();


    QDomDocument getDocument(const QDomNode& node) const;
    void scanTree(const QDomNode& node, void (SvgParser::*callback)(const QDomNode& node)const) const;

    // QDomNode parseSvgTree(const QDomNode& svgSkinNode) const;
    QDomNode parseSvgTree(const QDomNode& svgSkinNode);
    // QDomNode parseSvgFile(const QString& svgFileName) const;
    QDomNode parseSvgFile(const QString& svgFileName);
    QString saveToTempFile(const QDomNode& svgNode) const;
    QByteArray saveToQByteArray(const QDomNode& svgNode) const;
    void parseElement(const QDomNode& svgNode) const;


  private:
    void parseAttributes(const QDomNode& node) const;
    QScriptValue evaluateTemplateExpression(QString expression, int lineNumber) const;
    
    mutable SkinContext m_context;
    QDomDocument m_document;
    QString m_currentFile;
    
};

#endif /* SVGPARSER_H */
